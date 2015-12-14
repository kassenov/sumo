/****************************************************************************/
/// @file    MSSOTLMarchingPolicy.h
/// @author  Alessio Bonfietti
/// @author  Riccardo Belletti
/// @author  Federico Caselli
/// @date    Feb 2014
/// @version $Id: MSSOTLMarchingPolicy.cpp 19604 2015-12-13 20:49:24Z behrisch $
///
// The class for SOTL Marching logics
/****************************************************************************/
// SUMO, Simulation of Urban MObility; see http://sumo.dlr.de/
// Copyright 2001-2009 DLR (http://www.dlr.de/) and contributors
/****************************************************************************/
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation; either version 2 of the License, or
//   (at your option) any later version.
//
/****************************************************************************/

#include "MSSOTLMarchingPolicy.h"

MSSOTLMarchingPolicy::MSSOTLMarchingPolicy(
    const std::map<std::string, std::string>& parameters) :
    MSSOTLPolicy("Marching", parameters) {
    init();
}

MSSOTLMarchingPolicy::MSSOTLMarchingPolicy(
    MSSOTLPolicyDesirability* desirabilityAlgorithm) :
    MSSOTLPolicy("Marching", desirabilityAlgorithm) {
    getDesirabilityAlgorithm()->setKeyPrefix("MARCHING");
    init();
}

MSSOTLMarchingPolicy::MSSOTLMarchingPolicy(
    MSSOTLPolicyDesirability* desirabilityAlgorithm,
    const std::map<std::string, std::string>& parameters) :
    MSSOTLPolicy("Marching", desirabilityAlgorithm, parameters) {
    getDesirabilityAlgorithm()->setKeyPrefix("MARCHING");
    init();
}

bool MSSOTLMarchingPolicy::canRelease(int elapsed, bool thresholdPassed, bool pushButtonPressed,
                                      const MSPhaseDefinition* stage, int vehicleCount) {
    if (elapsed >= stage->minDuration && pushButtonLogic(elapsed, pushButtonPressed, stage)) {
        return true;
    }
    return (elapsed >= stage->duration);
}

void MSSOTLMarchingPolicy::init() {
    PushButtonLogic::init("MSSOTLMarchingPolicy", this);
}