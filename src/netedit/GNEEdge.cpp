/****************************************************************************/
/// @file    GNEEdge.cpp
/// @author  Jakob Erdmann
/// @date    Feb 2011
/// @version $Id: GNEEdge.cpp 21851 2016-10-31 12:20:12Z behrisch $
///
// A road/street connecting two junctions (netedit-version, adapted from GUIEdge)
// Basically a container for an NBEdge with drawing and editing capabilities
/****************************************************************************/
// SUMO, Simulation of Urban MObility; see http://sumo.dlr.de/
// Copyright (C) 2001-2016 DLR (http://www.dlr.de/) and contributors
/****************************************************************************/
//
//   This file is part of SUMO.
//   SUMO is free software: you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation, either version 3 of the License, or
//   (at your option) any later version.
//
/****************************************************************************/


// ===========================================================================
// included modules
// ===========================================================================
#ifdef _MSC_VER
#include <windows_config.h>
#else
#include <config.h>
#endif

#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#include <foreign/polyfonts/polyfonts.h>
#include <utils/gui/globjects/GUIGLObjectPopupMenu.h>
#include <utils/gui/settings/GUIVisualizationSettings.h>
#include <utils/geom/GeomHelper.h>
#include <utils/geom/GeomConvHelper.h>
#include <utils/gui/div/GUIParameterTableWindow.h>
#include <utils/gui/div/GLHelper.h>
#include <utils/gui/div/GUIGlobalSelection.h>
#include "GNEEdge.h"
#include "GNENet.h"
#include "GNEViewNet.h"
#include "GNEUndoList.h"
#include "GNEChange_Attribute.h"
#include "GNEChange_Lane.h"
#include "GNEJunction.h"
#include "GNELane.h"
#include "GNEAdditional.h"
#include "GNEAdditionalSet.h"
#include "GNEConnection.h"


#ifdef CHECK_MEMORY_LEAKS
#include <foreign/nvwa/debug_new.h>
#endif // CHECK_MEMORY_LEAKS



// ===========================================================================
// static
// ===========================================================================
const SUMOReal GNEEdge::SNAP_RADIUS = SUMO_const_halfLaneWidth;

// ===========================================================================
// members methods
// ===========================================================================
GNEEdge::GNEEdge(NBEdge& nbe, GNENet* net, bool wasSplit, bool loaded):
    GNENetElement(net, nbe.getID(), GLO_EDGE, SUMO_TAG_EDGE),
    myNBEdge(nbe) ,
    myOrigShape(nbe.getInnerGeometry()),
    myLanes(0),
    myAmResponsible(false),
    myWasSplit(wasSplit),
    myConnectionStatus(loaded ? LOADED : GUESSED) {
    // Create lanes
    int numLanes = myNBEdge.getNumLanes();
    myLanes.reserve(numLanes);
    for (int i = 0; i < numLanes; i++) {
        myLanes.push_back(new GNELane(*this, i));
        myLanes.back()->incRef("GNEEdge::GNEEdge");
    }
}


GNEEdge::~GNEEdge() {
    // Delete edges
    for (LaneVector::iterator i = myLanes.begin(); i != myLanes.end(); ++i) {
        (*i)->decRef("GNEEdge::~GNEEdge");
        if ((*i)->unreferenced()) {
            delete *i;
        }
    }
    // delete connections
    for (ConnectionVector::const_iterator i = myGNEConnections.begin(); i != myGNEConnections.end(); ++i) {
        (*i)->decRef("GNEEdge::~GNEEdge");
        if ((*i)->unreferenced()) {
            delete(*i);
        }
    }
    if (myAmResponsible) {
        delete &myNBEdge;
    }
}


void
GNEEdge::updateGeometry() {
    // Update geometry of lanes
    for (LaneVector::iterator i = myLanes.begin(); i != myLanes.end(); ++i) {
        (*i)->updateGeometry();
    }
    // Update geometry of additionals vinculated to this edge
    for (AdditionalVector::iterator i = myAdditionals.begin(); i != myAdditionals.end(); ++i) {
        (*i)->updateGeometry();
    }
    // Update geometry of additionalSets vinculated to this edge
    for (AdditionalSetVector::iterator i = myAdditionalSets.begin(); i != myAdditionalSets.end(); ++i) {
        (*i)->updateGeometry();
    }
}


Boundary
GNEEdge::getBoundary() const {
    Boundary ret;
    for (LaneVector::const_iterator i = myLanes.begin(); i != myLanes.end(); ++i) {
        ret.add((*i)->getBoundary());
    }
    ret.grow(10); // !!! magic value
    return ret;
}


Boundary
GNEEdge::getCenteringBoundary() const {
    Boundary b = getBoundary();
    b.grow(20);
    return b;
}


GUIGLObjectPopupMenu*
GNEEdge::getPopUpMenu(GUIMainWindow& app, GUISUMOAbstractView& parent) {
    GUIGLObjectPopupMenu* ret = new GUIGLObjectPopupMenu(app, parent, *this);
    buildPopupHeader(ret, app);
    buildCenterPopupEntry(ret);
    buildNameCopyPopupEntry(ret);
    buildSelectionPopupEntry(ret);
    buildPositionCopyEntry(ret, false);
    return ret;
}


GNEJunction*
GNEEdge::getGNEJunctionSource() const {
    return myNet->retrieveJunction(myNBEdge.getFromNode()->getID(), false);
}


GNEJunction*
GNEEdge::getGNEJunctionDest() const {
    return myNet->retrieveJunction(myNBEdge.getToNode()->getID(), false);
}

void
GNEEdge::drawGL(const GUIVisualizationSettings& s) const {
    /* do something different for connectors?
    if (myNBEdge.isMacroscopicConnector()) {
    }
    */

    // draw the lanes
    for (LaneVector::const_iterator i = myLanes.begin(); i != myLanes.end(); ++i) {
        (*i)->drawGL(s);
    }

    // draw the connections
    if (s.scale >= 2) {
        for (ConnectionVector::const_iterator i = myGNEConnections.begin(); i != myGNEConnections.end(); ++i) {
            (*i)->drawGL(s);
        }
    }

    // draw geometry hints
    if (s.scale * SNAP_RADIUS > 1.) { // check whether it is not too small
        GLHelper::setColor(s.junctionColorer.getSchemes()[0].getColor(2));
        if (gSelected.isSelected(getType(), getGlID()) && s.laneColorer.getActive() != 1) {
            // override with special colors (unless the color scheme is based on selection)
            GLHelper::setColor(GNENet::selectionColor.changedBrightness(-20));
        }
        // recognize full transparency and simply don't draw
        GLfloat color[4];
        glGetFloatv(GL_CURRENT_COLOR, color);
        if (color[3] > 0) {
            glPushName(getGlID());
            PositionVector geom = myNBEdge.getGeometry();
            for (int i = 1; i < (int)geom.size() - 1; i++) {
                Position pos = geom[i];
                glPushMatrix();
                glTranslated(pos.x(), pos.y(), GLO_JUNCTION - 0.01);
                GLHelper:: drawFilledCircle(SNAP_RADIUS, 32);
                glPopMatrix();
            }
            glPopName();
        }
    }

    // (optionally) draw the name and/or the street name
    const bool drawStreetName = s.streetName.show && myNBEdge.getStreetName() != "";
    if (s.edgeName.show || drawStreetName) {
        glPushName(getGlID());
        GNELane* lane1 = myLanes[0];
        GNELane* lane2 = myLanes[myLanes.size() - 1];
        Position p = lane1->getShape().positionAtOffset(lane1->getShape().length() / (SUMOReal) 2.);
        p.add(lane2->getShape().positionAtOffset(lane2->getShape().length() / (SUMOReal) 2.));
        p.mul(.5);
        SUMOReal angle = lane1->getShape().rotationDegreeAtOffset(lane1->getShape().length() / (SUMOReal) 2.);
        angle += 90;
        if (angle > 90 && angle < 270) {
            angle -= 180;
        }
        if (s.edgeName.show) {
            drawName(p, s.scale, s.edgeName, angle);
        }
        if (drawStreetName) {
            GLHelper::drawText(myNBEdge.getStreetName(), p, GLO_MAX,
                               s.streetName.size / s.scale, s.streetName.color, angle);
        }
        glPopName();
    }
}


GUIParameterTableWindow*
GNEEdge::getParameterWindow(GUIMainWindow& app,
                            GUISUMOAbstractView&) {
    GUIParameterTableWindow* ret = 0;
    UNUSED_PARAMETER(&app);
    return ret;
}


void
GNEEdge::updateJunctionPosition(GNEJunction* junction, const Position& origPos) {
    Position delta = junction->getNBNode()->getPosition() - origPos;
    PositionVector geom = myNBEdge.getGeometry();
    // geometry endpoint need not equal junction position hence we modify it with delta
    if (junction == getGNEJunctionSource()) {
        geom[0].add(delta);
    } else {
        geom[-1].add(delta);
    }
    setGeometry(geom, false);
}

NBEdge*
GNEEdge::getNBEdge() {
    return &myNBEdge;
}


Position
GNEEdge::getSplitPos(const Position& clickPos) {
    const PositionVector& geom = myNBEdge.getGeometry();
    int index = geom.indexOfClosest(clickPos);
    if (geom[index].distanceTo(clickPos) < SNAP_RADIUS) {
        // split at existing geometry point
        return geom[index];
    } else {
        // split straight between the next two points
        return geom.positionAtOffset(geom.nearest_offset_to_point2D(clickPos));
    }
}


Position
GNEEdge::moveGeometry(const Position& oldPos, const Position& newPos, bool relative) {
    PositionVector geom = myNBEdge.getGeometry();
    bool changed = changeGeometry(geom, getMicrosimID(), oldPos, newPos, relative);
    if (changed) {
        setGeometry(geom, false);
        return newPos;
    } else {
        return oldPos;
    }
}


bool
GNEEdge::changeGeometry(PositionVector& geom, const std::string& id, const Position& oldPos, const Position& newPos, bool relative, bool moveEndPoints) {
    if (geom.size() < 2) {
        throw ProcessError("Invalid geometry size in edge " + id);
    } else {
        int index = geom.indexOfClosest(oldPos);
        const SUMOReal nearestOffset = geom.nearest_offset_to_point2D(oldPos, true);
        if (nearestOffset != GeomHelper::INVALID_OFFSET
                && (moveEndPoints || (nearestOffset >= SNAP_RADIUS
                                      && nearestOffset <= geom.length2D() - SNAP_RADIUS))) {
            const Position nearest = geom.positionAtOffset2D(nearestOffset);
            const SUMOReal distance = geom[index].distanceTo2D(nearest);
            if (distance < SNAP_RADIUS) { //move existing
                if (moveEndPoints || (index != 0 && index != (int)geom.size() - 1)) {
                    const bool closed = geom.isClosed();
                    if (relative) {
                        geom[index] = geom[index] + newPos;
                    } else {
                        geom[index] = newPos;
                    }
                    if (closed && moveEndPoints && (index == 0 || index == (int)geom.size() - 1)) {
                        const int otherIndex = (int)geom.size() - 1 - index;
                        geom[otherIndex] = geom[index];
                    }
                    return true;
                }
            } else {
                if (relative) {
                    int index = geom.insertAtClosest(nearest);
                    geom[index] = geom[index] + newPos;
                    return true;
                } else {
                    geom.insertAtClosest(newPos); // insert new
                    return true;
                }
            }
        }
        return false;
    }
}


void
GNEEdge::moveGeometry(const Position& delta) {
    PositionVector geom = myNBEdge.getInnerGeometry();
    if (geom.size() == 0) {
        return;
    }
    geom.add(delta.x(), delta.y(), delta.z());
    setGeometry(geom, true);
}


bool
GNEEdge::deleteGeometry(const Position& pos, GNEUndoList* undoList) {
    PositionVector geom = myNBEdge.getInnerGeometry();
    if (geom.size() == 0) {
        return false;
    }
    int index = geom.indexOfClosest(pos);
    if (geom[index].distanceTo(pos) < SNAP_RADIUS) {
        geom.erase(geom.begin() + index);
        setAttribute(SUMO_ATTR_SHAPE, toString(geom), undoList);
        return true;
    } else {
        return false;
    }
}


void
GNEEdge::setEndpoint(Position pos, GNEUndoList* undoList) {
    undoList->p_begin("set endpoint");
    PositionVector geom = myNBEdge.getGeometry();
    int index = geom.indexOfClosest(pos);
    if (geom[index].distanceTo(pos) < SNAP_RADIUS) { // snap to existing geometry
        pos = geom[index];
    }
    Position destPos = getGNEJunctionDest()->getNBNode()->getPosition();
    Position sourcePos = getGNEJunctionSource()->getNBNode()->getPosition();
    if (pos.distanceTo2D(destPos) < pos.distanceTo2D(sourcePos)) {
        setAttribute(GNE_ATTR_SHAPE_END, toString(pos), undoList);
        getGNEJunctionDest()->invalidateShape();
    } else {
        setAttribute(GNE_ATTR_SHAPE_START, toString(pos), undoList);
        getGNEJunctionSource()->invalidateShape();
    }
    // possibly existing inner point is no longer needed
    deleteGeometry(pos, undoList);
    undoList->p_end();
}


void
GNEEdge::resetEndpoint(const Position& pos, GNEUndoList* undoList) {
    Position destPos = getGNEJunctionDest()->getNBNode()->getPosition();
    Position sourcePos = getGNEJunctionSource()->getNBNode()->getPosition();
    if (pos.distanceTo2D(destPos) < pos.distanceTo2D(sourcePos)) {
        setAttribute(GNE_ATTR_SHAPE_END, toString(destPos), undoList);
        getGNEJunctionDest()->invalidateShape();
    } else {
        setAttribute(GNE_ATTR_SHAPE_START, toString(sourcePos), undoList);
        getGNEJunctionSource()->invalidateShape();
    }
}


void
GNEEdge::setGeometry(PositionVector geom, bool inner) {
    myNBEdge.setGeometry(geom, inner);
    updateGeometry();
    getGNEJunctionSource()->invalidateShape();
    getGNEJunctionDest()->invalidateShape();
    myNet->refreshElement(this);
}

void
GNEEdge::remakeIncomingGNEConnections() {
    GNEJunction* from = getGNEJunctionSource();
    assert(from);
    std::vector<GNEEdge*> incomingEdges = from->getGNEIncomingEdges();
    for (std::vector<GNEEdge*>::iterator i = incomingEdges.begin(); i != incomingEdges.end(); i++) {
        (*i)->remakeGNEConnections();
    }
}

void
GNEEdge::remakeGNEConnections() {
    // @note: this method may only be called once the whole network is initialized
    // Create connections (but reuse existing objects)
    std::vector<NBEdge::Connection>& myConnections = myNBEdge.getConnections();
    ConnectionVector newCons;
    for (std::vector<NBEdge::Connection>::iterator i = myConnections.begin(); i != myConnections.end(); i++) {
        const NBEdge::Connection& con = *i;
        newCons.push_back(retrieveConnection(con.fromLane, con.toEdge, con.toLane));
        newCons.back()->incRef("GNEEdge::GNEEdge");
        newCons.back()->updateLinkState();
        //std::cout << " remakeGNEConnection " << newCons.back()->getNBConnection() << "\n";
    }
    clearGNEConnections();
    myGNEConnections = newCons;
}


void
GNEEdge::clearGNEConnections() {
    // Drop all existents connections that aren't referenced anymore
    for (ConnectionVector::iterator i = myGNEConnections.begin(); i != myGNEConnections.end(); i++) {
        // Dec reference of connection
        (*i)->decRef("GNEEdge::clearGNEConnections");
        // Delete GNEConnectionToErase if is unreferenced
        if ((*i)->unreferenced()) {
            delete(*i);
        }
    }
    myGNEConnections.clear();
}


void
GNEEdge::copyTemplate(GNEEdge* tpl, GNEUndoList* undoList) {
    undoList->p_begin("copy template");
    setAttribute(SUMO_ATTR_NUMLANES,   tpl->getAttribute(SUMO_ATTR_NUMLANES),  undoList);
    setAttribute(SUMO_ATTR_TYPE,       tpl->getAttribute(SUMO_ATTR_TYPE),     undoList);
    setAttribute(SUMO_ATTR_SPEED,      tpl->getAttribute(SUMO_ATTR_SPEED),    undoList);
    setAttribute(SUMO_ATTR_PRIORITY,   tpl->getAttribute(SUMO_ATTR_PRIORITY), undoList);
    setAttribute(SUMO_ATTR_SPREADTYPE, tpl->getAttribute(SUMO_ATTR_SPREADTYPE), undoList);
    setAttribute(SUMO_ATTR_WIDTH,      tpl->getAttribute(SUMO_ATTR_WIDTH), undoList);
    setAttribute(SUMO_ATTR_ENDOFFSET,     tpl->getAttribute(SUMO_ATTR_ENDOFFSET), undoList);
    // copy lane attributes as well
    for (int i = 0; i < (int)myLanes.size(); i++) {
        myLanes[i]->setAttribute(SUMO_ATTR_ALLOW, tpl->myLanes[i]->getAttribute(SUMO_ATTR_ALLOW), undoList);
        myLanes[i]->setAttribute(SUMO_ATTR_DISALLOW, tpl->myLanes[i]->getAttribute(SUMO_ATTR_DISALLOW), undoList);
        myLanes[i]->setAttribute(SUMO_ATTR_SPEED, tpl->myLanes[i]->getAttribute(SUMO_ATTR_SPEED), undoList);
        myLanes[i]->setAttribute(SUMO_ATTR_WIDTH, tpl->myLanes[i]->getAttribute(SUMO_ATTR_WIDTH), undoList);
        myLanes[i]->setAttribute(SUMO_ATTR_ENDOFFSET, tpl->myLanes[i]->getAttribute(SUMO_ATTR_ENDOFFSET), undoList);
    }
    undoList->p_end();
}


std::set<GUIGlID>
GNEEdge::getLaneGlIDs() {
    std::set<GUIGlID> result;
    for (size_t i = 0; i < myLanes.size(); i++) {
        result.insert(myLanes[i]->getGlID());
    }
    return result;
}


const std::vector<GNELane*>&
GNEEdge::getLanes() {
    return myLanes;
}


const std::vector<GNEConnection*>&
GNEEdge::getGNEConnections() {
    return myGNEConnections;
}


bool
GNEEdge::wasSplit() {
    return myWasSplit;
}


std::string
GNEEdge::getAttribute(SumoXMLAttr key) const {
    switch (key) {
        case SUMO_ATTR_ID:
            return getMicrosimID();
        case SUMO_ATTR_FROM:
            return getGNEJunctionSource()->getMicrosimID();
        case SUMO_ATTR_TO:
            return getGNEJunctionDest()->getMicrosimID();
        case SUMO_ATTR_NUMLANES:
            return toString(myNBEdge.getNumLanes());
        case SUMO_ATTR_PRIORITY:
            return toString(myNBEdge.getPriority());
        case SUMO_ATTR_LENGTH:
            return toString(myNBEdge.getFinalLength());
        case SUMO_ATTR_TYPE:
            return myNBEdge.getTypeID();
        case SUMO_ATTR_SHAPE:
            return toString(myNBEdge.getInnerGeometry());
        case SUMO_ATTR_SPREADTYPE:
            return toString(myNBEdge.getLaneSpreadFunction());
        case SUMO_ATTR_NAME:
            return myNBEdge.getStreetName();
        case SUMO_ATTR_ALLOW:
            // return all allowed classes (may differ from the written attributes)
            return (getVehicleClassNames(myNBEdge.getPermissions()) +
                    (myNBEdge.hasLaneSpecificPermissions() ? " (combined!)" : ""));
        case SUMO_ATTR_DISALLOW: {
            // return classes disallowed on at least one lane (may differ from the written attributes)
            SVCPermissions combinedDissallowed = 0;
            for (int i = 0; i < (int)myNBEdge.getNumLanes(); ++i) {
                combinedDissallowed |= ~myNBEdge.getPermissions(i);
            }
            return (getVehicleClassNames(combinedDissallowed) +
                    (myNBEdge.hasLaneSpecificPermissions() ? " (combined!)" : ""));
        }
        case SUMO_ATTR_SPEED:
            if (myNBEdge.hasLaneSpecificSpeed()) {
                return "lane specific";
            } else {
                return toString(myNBEdge.getSpeed());
            }
        case SUMO_ATTR_WIDTH:
            if (myNBEdge.hasLaneSpecificWidth()) {
                return "lane specific";
            } else {
                return toString(myNBEdge.getLaneWidth());
            }
        case SUMO_ATTR_ENDOFFSET:
            if (myNBEdge.hasLaneSpecificEndOffset()) {
                return "lane specific";
            } else {
                return toString(myNBEdge.getEndOffset());
            }
        case GNE_ATTR_MODIFICATION_STATUS:
            return myConnectionStatus;
        case GNE_ATTR_SHAPE_START:
            return toString(myNBEdge.getGeometry()[0]);
        case GNE_ATTR_SHAPE_END:
            return toString(myNBEdge.getGeometry()[-1]);
        default:
            throw InvalidArgument("edge attribute '" + toString(key) + "' not allowed");
    }
}


void
GNEEdge::setAttribute(SumoXMLAttr key, const std::string& value, GNEUndoList* undoList) {
    switch (key) {
        case SUMO_ATTR_WIDTH:
        case SUMO_ATTR_ENDOFFSET:
        case SUMO_ATTR_SPEED:
        case SUMO_ATTR_ALLOW:
        case SUMO_ATTR_DISALLOW: {
            undoList->p_begin("change edge attribute");
            const std::string origValue = getAttribute(key); // will have intermediate value of "lane specific"
            // lane specific attributes need to be changed via lanes to allow undo
            for (LaneVector::iterator it = myLanes.begin(); it != myLanes.end(); it++) {
                (*it)->setAttribute(key, value, undoList);
            }
            // ensure that the edge value is also changed. Actually this sets the lane attributes again but it does not matter
            undoList->p_add(new GNEChange_Attribute(this, key, value, true, origValue));
            undoList->p_end();
            break;
        }
        case SUMO_ATTR_FROM: {
            undoList->p_begin("change edge attribute");
            undoList->p_add(new GNEChange_Attribute(this, key, value));
            getGNEJunctionSource()->setLogicValid(false, undoList);
            myNet->retrieveJunction(value)->setLogicValid(false, undoList);
            setAttribute(GNE_ATTR_SHAPE_START, toString(getGNEJunctionSource()->getNBNode()->getPosition()), undoList);
            getGNEJunctionSource()->invalidateShape();
            undoList->p_end();
            break;
        }
        case SUMO_ATTR_TO: {
            undoList->p_begin("change edge attribute");
            undoList->p_add(new GNEChange_Attribute(this, key, value));
            getGNEJunctionDest()->setLogicValid(false, undoList);
            myNet->retrieveJunction(value)->setLogicValid(false, undoList);
            setAttribute(GNE_ATTR_SHAPE_END, toString(getGNEJunctionDest()->getNBNode()->getPosition()), undoList);
            getGNEJunctionDest()->invalidateShape();
            undoList->p_end();
            break;
        }
        case SUMO_ATTR_ID:
        case SUMO_ATTR_PRIORITY:
        case SUMO_ATTR_LENGTH:
        case SUMO_ATTR_TYPE:
        case SUMO_ATTR_SPREADTYPE:
        case GNE_ATTR_MODIFICATION_STATUS:
        case GNE_ATTR_SHAPE_START:
        case GNE_ATTR_SHAPE_END:
            undoList->p_add(new GNEChange_Attribute(this, key, value));
            break;
        case SUMO_ATTR_NAME:
            // user cares about street names. Make sure they appear in the output
            OptionsCont::getOptions().resetWritable();
            OptionsCont::getOptions().set("output.street-names", "true");
            undoList->p_add(new GNEChange_Attribute(this, key, value));
            break;
        case SUMO_ATTR_NUMLANES:
            if (value != getAttribute(key)) {
                setNumLanes(parse<int>(value), undoList);
            }
            break;
        case SUMO_ATTR_SHAPE:
            // @note: assumes value of inner geometry!
            // actually the geometry is already updated (incrementally
            // during mouse movement). We set the restore point to the end
            // of the last change-set
            myNBEdge.setGeometry(myOrigShape, true);
            undoList->p_add(new GNEChange_Attribute(this, key, value));
            break;
        default:
            throw InvalidArgument("edge attribute '" + toString(key) + "' not allowed");
    }
}


bool
GNEEdge::isValid(SumoXMLAttr key, const std::string& value) {
    switch (key) {
        case SUMO_ATTR_ID:
            return isValidID(value) && myNet->retrieveEdge(value, false) == 0;
            break;
        case SUMO_ATTR_FROM:
            return isValidID(value) && myNet->retrieveJunction(value, false) != 0 && value != getGNEJunctionDest()->getMicrosimID();
            break;
        case SUMO_ATTR_TO:
            return isValidID(value) && myNet->retrieveJunction(value, false) != 0 && value != getGNEJunctionSource()->getMicrosimID();
            break;
        case SUMO_ATTR_SPEED:
            return isPositive<SUMOReal>(value);
            break;
        case SUMO_ATTR_NUMLANES:
            return isPositive<int>(value);
            break;
        case SUMO_ATTR_PRIORITY:
            return canParse<int>(value);
            break;
        case SUMO_ATTR_LENGTH:
            return canParse<SUMOReal>(value) && (isPositive<SUMOReal>(value) || parse<SUMOReal>(value) == NBEdge::UNSPECIFIED_LOADED_LENGTH);
            break;
        case SUMO_ATTR_ALLOW:
        case SUMO_ATTR_DISALLOW:
            return canParseVehicleClasses(value);
            break;
        case SUMO_ATTR_TYPE:
            return true;
            break;
        case SUMO_ATTR_SHAPE: {
            bool ok = true;
            PositionVector shape = GeomConvHelper::parseShapeReporting(
                                       value, "user-supplied position", 0, ok, true);
            return ok;
            break;
        }
        case SUMO_ATTR_SPREADTYPE:
            return SUMOXMLDefinitions::LaneSpreadFunctions.hasString(value);
            break;
        case SUMO_ATTR_NAME:
            return true;
            break;
        case SUMO_ATTR_WIDTH:
            return canParse<SUMOReal>(value) && (isPositive<SUMOReal>(value) || parse<SUMOReal>(value) == NBEdge::UNSPECIFIED_WIDTH);
            break;
        case SUMO_ATTR_ENDOFFSET:
            return canParse<SUMOReal>(value);
            break;
        default:
            throw InvalidArgument("edge attribute '" + toString(key) + "' not allowed");
    }
}


void
GNEEdge::setResponsible(bool newVal) {
    myAmResponsible = newVal;
}

// ===========================================================================
// private
// ===========================================================================
void
GNEEdge::setAttribute(SumoXMLAttr key, const std::string& value) {
    switch (key) {
        case SUMO_ATTR_ID:
            myNet->renameEdge(this, value);
            break;
        case SUMO_ATTR_FROM:
            myNet->changeEdgeEndpoints(this, value, getGNEJunctionDest()->getMicrosimID());
            break;
        case SUMO_ATTR_TO:
            myNet->changeEdgeEndpoints(this, getGNEJunctionSource()->getMicrosimID(), value);
            break;
        case SUMO_ATTR_NUMLANES:
            throw InvalidArgument("GNEEdge::setAttribute (private) called for attr SUMO_ATTR_NUMLANES. This should never happen");
            break;
        case SUMO_ATTR_PRIORITY:
            myNBEdge.myPriority = parse<int>(value);
            break;
        case SUMO_ATTR_LENGTH:
            myNBEdge.setLoadedLength(parse<SUMOReal>(value));
            break;
        case SUMO_ATTR_TYPE:
            myNBEdge.myType = value;
            break;
        case SUMO_ATTR_SHAPE:
            bool ok;
            myOrigShape = GeomConvHelper::parseShapeReporting(value, "netedit-given", 0, ok, true);
            setGeometry(myOrigShape, true);
            break;
        case SUMO_ATTR_SPREADTYPE:
            myNBEdge.setLaneSpreadFunction(SUMOXMLDefinitions::LaneSpreadFunctions.get(value));
            break;
        case SUMO_ATTR_NAME:
            myNBEdge.setStreetName(value);
            break;
        case SUMO_ATTR_SPEED:
            myNBEdge.setSpeed(-1, parse<SUMOReal>(value));
            break;
        case SUMO_ATTR_WIDTH:
            myNBEdge.setLaneWidth(-1, parse<SUMOReal>(value));
            break;
        case SUMO_ATTR_ENDOFFSET:
            myNBEdge.setEndOffset(-1, parse<SUMOReal>(value));
            break;
        case SUMO_ATTR_ALLOW:
            break;  // no edge value
        case SUMO_ATTR_DISALLOW:
            break; // no edge value
        case GNE_ATTR_MODIFICATION_STATUS:
            myConnectionStatus = value;
            if (value == GUESSED) {
                myNBEdge.invalidateConnections(true);
            } else if (value != GUESSED) {
                myNBEdge.declareConnectionsAsLoaded();
            }
            break;
        case GNE_ATTR_SHAPE_START: {
            PositionVector geom = myNBEdge.getGeometry();
            geom[0] = GeomConvHelper::parseShapeReporting(value, "netedit-given", 0, ok, false)[0];
            setGeometry(geom, false);
            break;
        }
        case GNE_ATTR_SHAPE_END: {
            PositionVector geom = myNBEdge.getGeometry();
            geom[-1] = GeomConvHelper::parseShapeReporting(value, "netedit-given", 0, ok, false)[0];
            setGeometry(geom, false);
            break;
        }
        default:
            throw InvalidArgument("edge attribute '" + toString(key) + "' not allowed");
    }
}


void
GNEEdge::setNumLanes(int numLanes, GNEUndoList* undoList) {
    undoList->p_begin("change number of lanes");
    getGNEJunctionSource()->setLogicValid(false, undoList);
    getGNEJunctionDest()->setLogicValid(false, undoList);

    const int oldNumLanes = (int)myLanes.size();
    for (int i = oldNumLanes; i < numLanes; i++) {
        // since the GNELane does not exist yet, it cannot have yet been referenced so we only pass a zero-pointer
        undoList->add(new GNEChange_Lane(this, 0,
                                         myNBEdge.getLaneStruct(oldNumLanes - 1), true), true);
    }
    for (int i = oldNumLanes - 1; i > numLanes - 1; i--) {
        // delete leftmost lane
        undoList->add(new GNEChange_Lane(this, myLanes[i], myNBEdge.getLaneStruct(i), false), true);
    }
    undoList->p_end();
}


void
GNEEdge::addLane(GNELane* lane, const NBEdge::Lane& laneAttrs) {
    const int index = lane ? lane->getIndex() : myNBEdge.getNumLanes();
    // the laneStruct must be created first to ensure we have some geometry
    myNBEdge.addLane(index);
    if (lane) {
        // restore a previously deleted lane
        myLanes.insert(myLanes.begin() + index, lane);

    } else {
        // create a new lane by copying leftmost lane
        lane = new GNELane(*this, index);
        myLanes.push_back(lane);
    }
    lane->incRef("GNEEdge::addLane");
    // we copy all attributes except shape since this is recomputed from edge shape
    myNBEdge.setSpeed(lane->getIndex(), laneAttrs.speed);
    myNBEdge.setPermissions(laneAttrs.permissions, lane->getIndex());
    myNBEdge.setPreferredVehicleClass(laneAttrs.preferred, lane->getIndex());
    myNBEdge.setEndOffset(lane->getIndex(), laneAttrs.endOffset);
    myNBEdge.setLaneWidth(lane->getIndex(), laneAttrs.width);
    // udate indices
    for (int i = 0; i < (int)myLanes.size(); ++i) {
        myLanes[i]->setIndex(i);
    }
    /* while technically correct, this looks ugly
    getGNEJunctionSource()->invalidateShape();
    getGNEJunctionDest()->invalidateShape();
    */
    // Remake connections for this edge and all edges that target this lane
    remakeGNEConnections();
    remakeIncomingGNEConnections();
    // Update element
    myNet->refreshElement(this);
    updateGeometry();
}


void
GNEEdge::removeLane(GNELane* lane) {
    if (myLanes.size() == 0) {
        throw ProcessError("Should not remove the last lane from an edge\n");
    }
    if (lane == 0) {
        lane = myLanes.back();
    }
    // Delete lane of edge's container
    myNBEdge.deleteLane(lane->getIndex());
    lane->decRef("GNEEdge::removeLane");
    myLanes.erase(myLanes.begin() + lane->getIndex());
    // Delete lane if is unreferenced
    if (lane->unreferenced()) {
        delete lane;
    }
    // udate indices
    for (int i = 0; i < (int)myLanes.size(); ++i) {
        myLanes[i]->setIndex(i);
    }
    /* while technically correct, this looks ugly
    getGNEJunctionSource()->invalidateShape();
    getGNEJunctionDest()->invalidateShape();
    */
    // Remake connections for this edge and all edges that target this lane
    remakeGNEConnections();
    remakeIncomingGNEConnections();

    // Update element
    myNet->refreshElement(this);
    updateGeometry();
}

void
GNEEdge::addConnection(NBEdge::Connection nbCon, GNEConnection* con) {
    // If a new connection was sucesfully created
    if (myNBEdge.setConnection(nbCon.fromLane, nbCon.toEdge, nbCon.toLane, NBEdge::L2L_USER, true, nbCon.mayDefinitelyPass, nbCon.keepClear, nbCon.contPos, nbCon.visibility)) {
        // Create GNEConection
        con->updateGeometry();
        myGNEConnections.push_back(con);
        // Add reference
        myGNEConnections.back()->incRef("GNEEdge::addConnection");
    }
    myNet->refreshElement(this); // actually we only do this to force a redraw
}


void
GNEEdge::removeConnection(NBEdge::Connection nbCon) {

    if (nbCon.toEdge == myNBEdge.getTurnDestination()) {
        myNet->removeExplicitTurnaround(getMicrosimID());
    }
    // Get connection to remove
    GNEConnection* con = retrieveConnection(nbCon.fromLane, nbCon.toEdge, nbCon.toLane);
    myNBEdge.removeFromConnections(nbCon);
    if (!con->unreferenced()) {
        con->decRef("GNEEdge::removeConnection");
        myGNEConnections.erase(std::find(myGNEConnections.begin(), myGNEConnections.end(), con));
        myNet->refreshElement(this); // actually we only do this to force a redraw
    }
}


GNEConnection*
GNEEdge::retrieveConnection(int fromLane, NBEdge* to, int toLane) {
    for (ConnectionVector::iterator i = myGNEConnections.begin(); i != myGNEConnections.end(); ++i) {
        if ((*i)->getFromLaneIndex() == fromLane
                && (*i)->getEdgeTo()->getNBEdge() == to
                && (*i)->getToLaneIndex() == toLane) {
            return *i;
        }
    }
    return new GNEConnection(myLanes[fromLane], myNet->retrieveEdge(to->getID())->getLanes()[toLane]);
}



void
GNEEdge::setMicrosimID(const std::string& newID) {
    GUIGlObject::setMicrosimID(newID);
    for (LaneVector::iterator i = myLanes.begin(); i != myLanes.end(); ++i) {
        (*i)->setMicrosimID(getNBEdge()->getLaneID((*i)->getIndex()));
    }
}


void
GNEEdge::addAdditionalChild(GNEAdditional* additional) {
    // First check that additional wasn't already inserted
    for (AdditionalVector::iterator i = myAdditionals.begin(); i != myAdditionals.end(); i++) {
        if (*i == additional) {
            throw ProcessError("additional element with ID='" + additional->getID() + "' was already inserted in edge with ID='" + getID() + "'");
        }
    }
    myAdditionals.push_back(additional);
}


void
GNEEdge::removeAdditionalChild(GNEAdditional* additional) {
    // Declare iterator
    AdditionalVector::iterator i = myAdditionals.begin();
    // Find additional
    while ((*i != additional) && (i != myAdditionals.end())) {
        i++;
    }
    // If additional was found, remove it
    if (i == myAdditionals.end()) {
        throw ProcessError("additional element with ID='" + additional->getID() + "' doesn't exist in edge with ID='" + getID() + "'");
    } else {
        myAdditionals.erase(i);
    }
}


const std::vector<GNEAdditional*>&
GNEEdge::getAdditionalChilds() const {
    return myAdditionals;
}


bool
GNEEdge::addAdditionalSet(GNEAdditionalSet* additionalSet) {
    // Check if additionalSet already exists before insertion
    for (AdditionalSetVector::iterator i = myAdditionalSets.begin(); i != myAdditionalSets.end(); i++) {
        if ((*i) == additionalSet) {
            return false;
        }
    }
    // Insert it and retur true
    myAdditionalSets.push_back(additionalSet);
    return true;
}


bool
GNEEdge::removeAdditionalGeometrySet(GNEAdditionalSet* additionalSet) {
    // search additionalSet and remove it
    for (AdditionalSetVector::iterator i = myAdditionalSets.begin(); i != myAdditionalSets.end(); i++) {
        if ((*i) == additionalSet) {
            myAdditionalSets.erase(i);
            return true;
        }
    }
    // If additionalSet wasn't found, return false
    return false;
}


const std::vector<GNEAdditionalSet*>&
GNEEdge::getAdditionalSets() {
    return myAdditionalSets;
}


bool
GNEEdge::hasRestrictedLane(SUMOVehicleClass vclass) const {
    for (std::vector<GNELane*>::const_iterator i = myLanes.begin(); i != myLanes.end(); i++) {
        if ((*i)->isRestricted(vclass)) {
            return true;
        }
    }
    return false;
}

/****************************************************************************/
