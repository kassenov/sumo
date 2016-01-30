/****************************************************************************/
/// @file    MESegment.cpp
/// @author  Daniel Krajzewicz
/// @date    Tue, May 2005
/// @version $Id: MESegment.cpp 19820 2016-01-28 08:48:03Z bieker $
///
// A single mesoscopic segment (cell)
/****************************************************************************/
// SUMO, Simulation of Urban MObility; see http://sumo.dlr.de/
// Copyright (C) 2001-2015 DLR (http://www.dlr.de/) and contributors
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

#include <algorithm>
#include <limits>
#include <utils/common/StdDefs.h>
#include <microsim/MSGlobals.h>
#include <microsim/MSEdge.h>
#include <microsim/MSNet.h>
#include <microsim/MSLane.h>
#include <microsim/MSLinkCont.h>
#include <microsim/MSVehicle.h>
#include <microsim/MSMoveReminder.h>
#include <microsim/output/MSXMLRawOut.h>
#include <microsim/MSVehicleControl.h>
#include <microsim/devices/MSDevice.h>
#include <utils/common/FileHelpers.h>
#include <utils/iodevices/BinaryInputDevice.h>
#include <utils/iodevices/OutputDevice.h>
#include <utils/common/RandHelper.h>
#include "MEVehicle.h"
#include "MELoop.h"
#include "MESegment.h"

#ifdef CHECK_MEMORY_LEAKS
#include <foreign/nvwa/debug_new.h>
#endif // CHECK_MEMORY_LEAKS

// ===========================================================================
// static member defintion
// ===========================================================================
MSEdge MESegment::myDummyParent("MESegmentDummyParent", -1, MSEdge::EDGEFUNCTION_UNKNOWN, "", "", -1);
MESegment MESegment::myVaporizationTarget("vaporizationTarget");
const SUMOReal MESegment::DO_NOT_PATCH_JAM_THRESHOLD(std::numeric_limits<SUMOReal>::max());

// ===========================================================================
// method definitions
// ===========================================================================
MESegment::MESegment(const std::string& id,
                     const MSEdge& parent, MESegment* next,
                     SUMOReal length, SUMOReal speed,
                     unsigned int idx,
                     SUMOTime tauff, SUMOTime taufj,
                     SUMOTime taujf, SUMOTime taujj,
                     SUMOReal jamThresh, bool multiQueue, bool junctionControl,
                     SUMOReal lengthGeometryFactor) :
    Named(id), myEdge(parent), myNextSegment(next),
    myLength(length), myMaxSpeed(speed), myIndex(idx),
    myTau_ff((SUMOTime)(tauff / parent.getLanes().size())),
    myTau_fj((SUMOTime)(taufj / parent.getLanes().size())), // Eissfeldt p. 90 and 151 ff.
    myTau_jf((SUMOTime)(taujf / parent.getLanes().size())),
    myTau_jj((SUMOTime)(taujj / parent.getLanes().size())),
    myHeadwayCapacity(length / 7.5f * parent.getLanes().size())/* Eissfeldt p. 69 */,
    myCapacity(length * parent.getLanes().size()),
    myOccupancy(0.f), 
    myJunctionControl(junctionControl),
    myEntryBlockTime(SUMOTime_MIN),
    myLengthGeometryFactor(lengthGeometryFactor),
    myMeanSpeed(speed),
    myLastMeanSpeedUpdate(SUMOTime_MIN) 
{
    myCarQues.push_back(std::vector<MEVehicle*>());
    myBlockTimes.push_back(-1);
    const std::vector<MSLane*>& lanes = parent.getLanes();
    if (multiQueue && lanes.size() > 1) {
        unsigned int numFollower = parent.getNumSuccessors();
        if (numFollower > 1) {
            while (myCarQues.size() < lanes.size()) {
                myCarQues.push_back(std::vector<MEVehicle*>());
                myBlockTimes.push_back(-1);
            }
            for (unsigned int i = 0; i < numFollower; ++i) {
                const MSEdge* edge = parent.getSuccessors()[i];
                myFollowerMap[edge] = std::vector<size_t>();
                const std::vector<MSLane*>* allowed = parent.allowedLanes(*edge);
                assert(allowed != 0);
                assert(allowed->size() > 0);
                for (std::vector<MSLane*>::const_iterator j = allowed->begin(); j != allowed->end(); ++j) {
                    std::vector<MSLane*>::const_iterator it = find(lanes.begin(), lanes.end(), *j);
                    myFollowerMap[edge].push_back(distance(lanes.begin(), it));
                }
            }
        }
    }
    recomputeJamThreshold(jamThresh);
}


MESegment::MESegment(const std::string& id):
    Named(id),
    myEdge(myDummyParent), // arbitrary edge needed to supply the needed reference
    myNextSegment(0), myLength(0), myMaxSpeed(0), myIndex(0),
    myTau_ff(0), myTau_fj(0), myTau_jf(0), myTau_jj(0),
    myHeadwayCapacity(0), myCapacity(0), myJunctionControl(false),
    myLengthGeometryFactor(0)
{}


void
MESegment::recomputeJamThreshold(SUMOReal jamThresh) {
    if (jamThresh == DO_NOT_PATCH_JAM_THRESHOLD) {
        return;
    }
    if (jamThresh < 0) {
        // compute based on speed
        myJamThreshold = jamThresholdForSpeed(myMaxSpeed);
    } else {
        // compute based on specified percentage
        myJamThreshold = jamThresh * myCapacity;
    }
}


SUMOReal
MESegment::jamThresholdForSpeed(SUMOReal speed) const {
    // vehicles driving freely at maximum speed should not jam
    // we compute how many vehicles could possible enter the segment until the first vehicle leaves
    // and multiply by the space these vehicles would occupy
    return std::ceil((myLength / (speed * STEPS2TIME(myTau_ff)))) * (SUMOVTypeParameter::getDefault().length + SUMOVTypeParameter::getDefault().minGap);
}


void
MESegment::addDetector(MSMoveReminder* data) {
    myDetectorData.push_back(data);
    for (Queues::const_iterator k = myCarQues.begin(); k != myCarQues.end(); ++k) {
        for (std::vector<MEVehicle*>::const_reverse_iterator i = k->rbegin(); i != k->rend(); ++i) {
            (*i)->addReminder(data);
        }
    }
}


void
MESegment::removeDetector(MSMoveReminder* data) {
    std::vector<MSMoveReminder*>::iterator it = find(
                myDetectorData.begin(), myDetectorData.end(), data);
    if (it != myDetectorData.end()) {
        myDetectorData.erase(it);
    }
    for (Queues::const_iterator k = myCarQues.begin(); k != myCarQues.end(); ++k) {
        for (std::vector<MEVehicle*>::const_reverse_iterator i = k->rbegin(); i != k->rend(); ++i) {
            (*i)->removeReminder(data);
        }
    }
}


void
MESegment::updateDetectorsOnLeave(MEVehicle* v, SUMOTime currentTime, MESegment* next) {
    MSMoveReminder::Notification reason;
    if (next == 0) {
        reason = MSMoveReminder::NOTIFICATION_ARRIVED;
    } else if (next == &myVaporizationTarget) {
        reason = MSMoveReminder::NOTIFICATION_VAPORIZED;
    } else if (myNextSegment == 0) {
        reason = MSMoveReminder::NOTIFICATION_JUNCTION;
    } else {
        reason = MSMoveReminder::NOTIFICATION_SEGMENT;
    }
    v->updateDetectors(currentTime, true, reason);
}


void
MESegment::prepareDetectorForWriting(MSMoveReminder& data) {
    const SUMOTime currentTime = MSNet::getInstance()->getCurrentTimeStep();
    for (Queues::const_iterator k = myCarQues.begin(); k != myCarQues.end(); ++k) {
        SUMOTime earliestExitTime = currentTime;
        for (std::vector<MEVehicle*>::const_reverse_iterator i = k->rbegin(); i != k->rend(); ++i) {
            const SUMOTime exitTime = MAX2(earliestExitTime, (*i)->getEventTime());
            (*i)->updateDetectorForWriting(&data, currentTime, exitTime);
            earliestExitTime = exitTime + myTau_ff;
        }
    }
}


bool
MESegment::hasSpaceFor(const MEVehicle* veh, SUMOTime entryTime, bool init) const {
    if (myOccupancy == 0.) {
        // we have always space for at least one vehicle
        return true;
    }
    const SUMOReal newOccupancy = myOccupancy + veh->getVehicleType().getLengthWithGap();
    if (newOccupancy > myCapacity) {
        // we must ensure that occupancy remains below capacity
        return false;
    }
    // regular insertions and initial insertions must respect different constraints:
    // - regular insertions must respect entryBlockTime
    // - initial insertions should not cause additional jamming
    if (init) {
        // inserted vehicle should be able to continue at the current speed
        return newOccupancy <= jamThresholdForSpeed(getMeanSpeed(false));
    }
    // maintain propper spacing between inflow from different lanes
    return entryTime >= myEntryBlockTime;
}


bool
MESegment::initialise(MEVehicle* veh, SUMOTime time) {
    if (hasSpaceFor(veh, time, true)) {
        receive(veh, time, true);
        // we can check only after insertion because insertion may change the route via devices
        std::string msg;
        if (MSGlobals::gCheckRoutes && !veh->hasValidRoute(msg)) {
            throw ProcessError("Vehicle '" + veh->getID() + "' has no valid route. " + msg);
        }
        return true;
    }
    return false;
}


size_t
MESegment::getCarNumber() const {
    size_t total = 0;
    for (Queues::const_iterator k = myCarQues.begin(); k != myCarQues.end(); ++k) {
        total += k->size();
    }
    return total;
}


SUMOReal
MESegment::getMeanSpeed(bool useCached) const {
    const SUMOTime currentTime = MSNet::getInstance()->getCurrentTimeStep();
    if (currentTime != myLastMeanSpeedUpdate || !useCached) {
        myLastMeanSpeedUpdate = currentTime;
        const SUMOTime tau = free() ? myTau_ff : myTau_jf;
        SUMOReal v = 0;
        size_t count = 0;
        for (Queues::const_iterator k = myCarQues.begin(); k != myCarQues.end(); ++k) {
            SUMOTime earliestExitTime = currentTime;
            count += k->size();
            for (std::vector<MEVehicle*>::const_reverse_iterator veh = k->rbegin(); veh != k->rend(); ++veh) {
                v += (*veh)->getConservativeSpeed(earliestExitTime); // earliestExitTime is updated!
                earliestExitTime += tau;
            }
        }
        if (count == 0) {
            myMeanSpeed = myMaxSpeed;
        } else {
            myMeanSpeed = v / (SUMOReal) count;
        }
    }
    return myMeanSpeed;
}


void
MESegment::writeVehicles(OutputDevice& of) const {
    for (Queues::const_iterator k = myCarQues.begin(); k != myCarQues.end(); ++k) {
        for (std::vector<MEVehicle*>::const_iterator veh = k->begin(); veh != k->end(); ++veh) {
            MSXMLRawOut::writeVehicle(of, *(*veh));
        }
    }
}


MEVehicle*
MESegment::removeCar(MEVehicle* v, SUMOTime leaveTime, MESegment* next) {
    myOccupancy = MAX2((SUMOReal)0, myOccupancy - v->getVehicleType().getLengthWithGap());
    std::vector<MEVehicle*>& cars = myCarQues[v->getQueIndex()];
    assert(std::find(cars.begin(), cars.end(), v) != cars.end());
    // One could be tempted to do  v->setSegment(next); here but position on lane will be invalid if next == 0
    updateDetectorsOnLeave(v, leaveTime, next);
    if (v == cars.back()) {
        cars.pop_back();
        if (!cars.empty()) {
            return cars.back();
        }
    } else {
        cars.erase(std::find(cars.begin(), cars.end(), v));
    }
    return 0;
}


SUMOTime
MESegment::getTimeHeadway(bool predecessorIsFree) {
    if (predecessorIsFree) {
        return free() ? myTau_ff : myTau_fj;
    } else {
        if (free()) {
            return myTau_jf;
        } else {
            // the gap has to move from the start of the segment to its end
            // this allows jams to clear and move upstream
            const SUMOTime b = (SUMOTime)(myHeadwayCapacity * (myTau_jf - myTau_jj));
            return (SUMOTime)(myTau_jj * getCarNumber() + b);
        }
    }
}


SUMOTime
MESegment::getNextInsertionTime(SUMOTime earliestEntry) const {
    // since we do not know which queue will be used we give a conservative estimate
    SUMOTime earliestLeave = earliestEntry;
    for (size_t i = 0; i < myCarQues.size(); ++i) {
        earliestLeave = MAX2(earliestLeave, myBlockTimes[i]);
    }
    return MAX3(earliestEntry, earliestLeave - TIME2STEPS(myLength / myMaxSpeed), myEntryBlockTime);
}


MSLink*
MESegment::getLink(const MEVehicle* veh) const {
    if (myJunctionControl) {
        const MSEdge* const nextEdge = veh->succEdge(1);
        if (nextEdge == 0) {
            return 0;
        }
        // try to find any link leading to our next edge, start with the lane pointed to by the que index
        const MSLane* const bestLane = myEdge.getLanes()[veh->getQueIndex()];
        const MSLinkCont& links = bestLane->getLinkCont();
        for (std::vector<MSLink*>::const_iterator j = links.begin(); j != links.end(); ++j) {
            if (&(*j)->getLane()->getEdge() == nextEdge) {
                return *j;
            }
        }
        // this is for the non-multique case, maybe we should use caching here !!!
        for (std::vector<MSLane*>::const_iterator l = myEdge.getLanes().begin(); l != myEdge.getLanes().end(); ++l) {
            if ((*l) != bestLane) {
                const MSLinkCont& links = (*l)->getLinkCont();
                for (std::vector<MSLink*>::const_iterator j = links.begin(); j != links.end(); ++j) {
                    if (&(*j)->getLane()->getEdge() == nextEdge) {
                        return *j;
                    }
                }
            }
        }
    }
    return 0;
}


bool
MESegment::isOpen(const MEVehicle* veh) const {
    const MSLink* link = getLink(veh);
    return (link == 0
            || link->havePriority()
            || limitedControlOverride(link)
            || link->opened(veh->getEventTime(), veh->getSpeed(), veh->getSpeed(),
                            veh->getVehicleType().getLengthWithGap(), veh->getImpatience(),
                            veh->getVehicleType().getCarFollowModel().getMaxDecel(), veh->getWaitingTime()));
}


bool
MESegment::limitedControlOverride(const MSLink* link) const {
    assert(link != 0);
    if (!MSGlobals::gMesoLimitedJunctionControl) {
        return false;
    }
    // if the target segment of this link is not saturated junction control is disabled
    const MSEdge& targetEdge = link->getLane()->getEdge();
    const MESegment* target = MSGlobals::gMesoNet->getSegmentForEdge(targetEdge);
    return target->myOccupancy * 2 < target->myJamThreshold;
}


void
MESegment::send(MEVehicle* veh, MESegment* next, SUMOTime time) {
    assert(isInvalid(next) || time >= myBlockTimes[veh->getQueIndex()]);
    MSLink* link = getLink(veh);
    if (link != 0) {
        link->removeApproaching(veh);
    }
    MEVehicle* lc = removeCar(veh, time, next); // new leaderCar
    myBlockTimes[veh->getQueIndex()] = time;
    if (!isInvalid(next)) {
        myBlockTimes[veh->getQueIndex()] += next->getTimeHeadway(free());
    }
    if (lc != 0) {
        lc->setEventTime(MAX2(lc->getEventTime(), myBlockTimes[veh->getQueIndex()]));
        MSGlobals::gMesoNet->addLeaderCar(lc, getLink(lc));
    }
}

bool
MESegment::overtake() {
    return MSGlobals::gMesoOvertaking && myCapacity > myLength && RandHelper::rand() > (myOccupancy / myCapacity);
}


void
MESegment::addReminders(MEVehicle* veh) const {
    for (std::vector<MSMoveReminder*>::const_iterator i = myDetectorData.begin(); i != myDetectorData.end(); ++i) {
        veh->addReminder(*i);
    }
}

void
MESegment::receive(MEVehicle* veh, SUMOTime time, bool isDepart, bool afterTeleport) {
    const SUMOReal speed = isDepart ? -1 : veh->getSpeed(); // on the previous segment
    veh->setSegment(this); // for arrival checking
    veh->setLastEntryTime(time);
    veh->setBlockTime(SUMOTime_MAX);
    if (!isDepart && (
                // arrival on entering a new edge
                ((myIndex == 0 || afterTeleport) && veh->moveRoutePointer())
                // arrival on entering a new segment
                || veh->hasArrived())) {
        // route has ended
        veh->setEventTime(time + TIME2STEPS(myLength / speed)); // for correct arrival speed
        addReminders(veh);
        veh->activateReminders(MSMoveReminder::NOTIFICATION_JUNCTION);
        updateDetectorsOnLeave(veh, time, 0);
        MSNet::getInstance()->getVehicleControl().scheduleVehicleRemoval(veh);
        return;
    }
    // route continues
    const SUMOReal maxSpeedOnEdge = veh->getChosenSpeedFactor() * myMaxSpeed;
    const SUMOReal uspeed = MAX2(MIN2(maxSpeedOnEdge, veh->getVehicleType().getMaxSpeed()), (SUMOReal).05);
    size_t nextQueIndex = 0;
    if (myCarQues.size() > 1) {
        const MSEdge* succ = veh->succEdge(1);
        // succ may be invalid if called from initialise() with an invalid route
        if (succ != 0 && myFollowerMap.count(succ) > 0) {
            const std::vector<size_t>& indices = myFollowerMap[succ];
            nextQueIndex = indices[0];
            for (std::vector<size_t>::const_iterator i = indices.begin() + 1; i != indices.end(); ++i) {
                if (myCarQues[*i].size() < myCarQues[nextQueIndex].size()) {
                    nextQueIndex = *i;
                }
            }
        }
    }
    std::vector<MEVehicle*>& cars = myCarQues[nextQueIndex];
    MEVehicle* newLeader = 0; // first vehicle in the current queue
    SUMOTime tleave = MAX2(time + TIME2STEPS(myLength / uspeed) + veh->getStoptime(this), myBlockTimes[nextQueIndex]);
    if (cars.empty()) {
        cars.push_back(veh);
        newLeader = veh;
    } else {
        SUMOTime leaderOut = cars[0]->getEventTime();
        if (!isDepart && leaderOut > tleave && overtake()) {
            if (cars.size() == 1) {
                MSGlobals::gMesoNet->removeLeaderCar(cars[0]);
                newLeader = veh;
            }
            cars.insert(cars.begin() + 1, veh);
        } else {
            tleave = MAX2(leaderOut + myTau_ff, tleave);
            cars.insert(cars.begin(), veh);
        }
    }
    if (!isDepart) {
        // regular departs could take place anywhere on the edge so they should not block regular flow
        // the -1 facilitates interleaving of multiple streams
        myEntryBlockTime = time + myTau_ff - 1;
    }
    veh->setEventTime(tleave, tleave > time + TIME2STEPS(myLength / maxSpeedOnEdge));
    veh->setSegment(this, nextQueIndex);
    myOccupancy = MIN2(myCapacity, myOccupancy + veh->getVehicleType().getLengthWithGap());
    addReminders(veh);
    if (isDepart || myIndex == 0 || afterTeleport) {
        veh->activateReminders(isDepart ? MSMoveReminder::NOTIFICATION_DEPARTED : MSMoveReminder::NOTIFICATION_JUNCTION);
    } else {
        veh->activateReminders(MSMoveReminder::NOTIFICATION_SEGMENT);
    }
    if (newLeader != 0) {
        MSGlobals::gMesoNet->addLeaderCar(newLeader, getLink(newLeader));
    }
}


bool
MESegment::vaporizeAnyCar(SUMOTime currentTime) {
    MEVehicle* remove = 0;
    for (Queues::const_iterator k = myCarQues.begin(); k != myCarQues.end(); ++k) {
        if (!k->empty()) {
            // remove last in queue
            remove = k->front();
            if (k->size() == 1) {
                MSGlobals::gMesoNet->removeLeaderCar(remove);
            }
            MSGlobals::gMesoNet->changeSegment(remove, currentTime, &myVaporizationTarget);
            return true;
        }
    }
    return false;
}


void
MESegment::setSpeedForQueue(SUMOReal newSpeed, SUMOTime currentTime, SUMOTime blockTime, const std::vector<MEVehicle*>& vehs) {
    MEVehicle* v = vehs.back();
    v->updateDetectors(currentTime, false);
    SUMOTime newEvent = MAX2(newArrival(v, newSpeed, currentTime), blockTime);
    if (v->getEventTime() != newEvent) {
        MSGlobals::gMesoNet->removeLeaderCar(v);
        v->setEventTime(newEvent);
        MSGlobals::gMesoNet->addLeaderCar(v, getLink(v));
    }
    for (std::vector<MEVehicle*>::const_reverse_iterator i = vehs.rbegin() + 1; i != vehs.rend(); ++i) {
        (*i)->updateDetectors(currentTime, false);
        newEvent = MAX2(newArrival(*i, newSpeed, currentTime), newEvent + myTau_ff);
        (*i)->setEventTime(newEvent);
    }
}


SUMOTime
MESegment::newArrival(const MEVehicle* const v, SUMOReal newSpeed, SUMOTime currentTime) {
    // since speed is only an upper bound pos may be to optimistic
    const SUMOReal pos = MIN2(myLength, STEPS2TIME(currentTime - v->getLastEntryTime()) * v->getSpeed());
    // traveltime may not be 0
    return currentTime + MAX2(TIME2STEPS((myLength - pos) / newSpeed), SUMOTime(1));
}


void
MESegment::setSpeed(SUMOReal newSpeed, SUMOTime currentTime, SUMOReal jamThresh) {
    if (myMaxSpeed == newSpeed) {
        return;
    }
    myMaxSpeed = newSpeed;
    recomputeJamThreshold(jamThresh);
    for (size_t i = 0; i < myCarQues.size(); ++i) {
        if (myCarQues[i].size() != 0) {
            setSpeedForQueue(newSpeed, currentTime, myBlockTimes[i], myCarQues[i]);
        }
    }
}


SUMOTime
MESegment::getEventTime() const {
    SUMOTime result = SUMOTime_MAX;
    for (size_t i = 0; i < myCarQues.size(); ++i) {
        if (myCarQues[i].size() != 0 && myCarQues[i].back()->getEventTime() < result) {
            result = myCarQues[i].back()->getEventTime();
        }
    }
    if (result < SUMOTime_MAX) {
        return result;
    }
    return -1;
}


void
MESegment::saveState(OutputDevice& out) {
    out.openTag(SUMO_TAG_SEGMENT);
    for (size_t i = 0; i < myCarQues.size(); ++i) {
        out.openTag(SUMO_TAG_VIEWSETTINGS_VEHICLES).writeAttr(SUMO_ATTR_TIME, toString<SUMOTime>(myBlockTimes[i]));
        out.writeAttr(SUMO_ATTR_VALUE, myCarQues[i]);
        out.closeTag();
    }
    out.closeTag();
}


void
MESegment::loadState(std::vector<std::string>& vehIds, MSVehicleControl& vc, const SUMOTime block, const unsigned int queIdx) {
    for (std::vector<std::string>::const_iterator it = vehIds.begin(); it != vehIds.end(); ++it) {
        MEVehicle* v = static_cast<MEVehicle*>(vc.getVehicle(*it));
        assert(v != 0);
        assert(v->getSegment() == this);
        myCarQues[queIdx].push_back(v);
        myOccupancy += v->getVehicleType().getLengthWithGap();
    }
    if (myCarQues[queIdx].size() != 0) {
        // add the last vehicle of this queue
        // !!! one question - what about the previously added vehicle? Is it stored twice?
        MEVehicle* veh = myCarQues[queIdx].back();
        MSGlobals::gMesoNet->addLeaderCar(veh, getLink(veh));
    }
    myBlockTimes[queIdx] = block;
    myOccupancy = MIN2(myCapacity, myOccupancy);
}


std::vector<const MEVehicle*>
MESegment::getVehicles() const {
    std::vector<const MEVehicle*> result;
    for (Queues::const_iterator k = myCarQues.begin(); k != myCarQues.end(); ++k) {
        result.insert(result.end(), k->begin(), k->end());
    }
    return result;
}


SUMOReal
MESegment::getFlow() const {
    return 3600 * getCarNumber() * getMeanSpeed() / myLength;
}


/****************************************************************************/