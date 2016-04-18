# -*- coding: utf-8 -*-
"""
@file    connection.py
@author  Michael Behrisch
@author  Lena Kalleske
@author  Mario Krumnow
@author  Daniel Krajzewicz
@author  Jakob Erdmann
@date    2008-10-09
@version $Id: connection.py 20433 2016-04-13 08:00:14Z behrisch $

Python implementation of the TraCI interface.

SUMO, Simulation of Urban MObility; see http://sumo.dlr.de/
Copyright (C) 2008-2016 DLR (http://www.dlr.de/) and contributors

This file is part of SUMO.
SUMO is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.
"""
from __future__ import print_function
from __future__ import absolute_import
import socket
import struct

try:
    import traciemb
    _embedded = True
except ImportError:
    _embedded = False
from . import constants as tc
from .exceptions import TraCIException, FatalTraCIError
from .domain import _defaultDomains
from .storage import Storage

_RESULTS = {0x00: "OK", 0x01: "Not implemented", 0xFF: "Error"}


class Connection:

    """Contains the socket, the composed message string
    together with a list of TraCI commands which are inside.
    """
    def __init__(self, host, port):
        if not _embedded:
            self._socket = socket.socket()
            self._socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._socket.connect((host, port))
        self._string = bytes()
        self._queue = []
        self._subscriptionMapping = {}
        for domain in _defaultDomains:
            domain._register(self, self._subscriptionMapping)

    def _packString(self, s, pre=tc.TYPE_STRING):
        self._string += struct.pack("!Bi", pre, len(s)) + s.encode("latin1")

    def _packStringList(self, l):
        self._string += struct.pack("!Bi", tc.TYPE_STRINGLIST, len(l))
        for s in l:
            self._string += struct.pack("!i", len(s)) + s.encode("latin1")

    def _recvExact(self):
        try:
            result = bytes()
            while len(result) < 4:
                t = self._socket.recv(4 - len(result))
                if not t:
                    return None
                result += t
            length = struct.unpack("!i", result)[0] - 4
            result = bytes()
            while len(result) < length:
                t = self._socket.recv(length - len(result))
                if not t:
                    return None
                result += t
            return Storage(result)
        except socket.error:
            return None

    def _sendExact(self):
        if _embedded:
            result = Storage(traciemb.execute(self._string))
        else:
            length = struct.pack("!i", len(self._string) + 4)
            self._socket.send(length + self._string)
            result = self._recvExact()
        if not result:
            self._socket.close()
            del self._socket
            raise FatalTraCIError("connection closed by SUMO")
        for command in self._queue:
            prefix = result.read("!BBB")
            err = result.readString()
            if prefix[2] or err:
                self._string = bytes()
                self._queue = []
                raise TraCIException(prefix[1], _RESULTS[prefix[2]], err)
            elif prefix[1] != command:
                raise FatalTraCIError("Received answer %s for command %s." % (prefix[1],
                                                                              command))
            elif prefix[1] == tc.CMD_STOP:
                length = result.read("!B")[0] - 1
                result.read("!%sx" % length)
        self._string = bytes()
        self._queue = []
        return result

    def _beginMessage(self, cmdID, varID, objID, length=0):
        self._queue.append(cmdID)
        length += 1 + 1 + 1 + 4 + len(objID)
        if length <= 255:
            self._string += struct.pack("!BB", length, cmdID)
        else:
            self._string += struct.pack("!BiB", 0, length + 4, cmdID)
        self._packString(objID, varID)

    def _sendReadOneStringCmd(self, cmdID, varID, objID):
        self._beginMessage(cmdID, varID, objID)
        return self._checkResult(cmdID, varID, objID)

    def _sendIntCmd(self, cmdID, varID, objID, value):
        self._beginMessage(cmdID, varID, objID, 1 + 4)
        self._string += struct.pack("!Bi", tc.TYPE_INTEGER, value)
        self._sendExact()

    def _sendDoubleCmd(self, cmdID, varID, objID, value):
        self._beginMessage(cmdID, varID, objID, 1 + 8)
        self._string += struct.pack("!Bd", tc.TYPE_DOUBLE, value)
        self._sendExact()

    def _sendByteCmd(self, cmdID, varID, objID, value):
        self._beginMessage(cmdID, varID, objID, 1 + 1)
        self._string += struct.pack("!BB", tc.TYPE_BYTE, value)
        self._sendExact()

    def _sendStringCmd(self, cmdID, varID, objID, value):
        self._beginMessage(cmdID, varID, objID, 1 + 4 + len(value))
        self._packString(value)
        self._sendExact()

    def _checkResult(self, cmdID, varID, objID):
        result = self._sendExact()
        result.readLength()
        response, retVarID = result.read("!BB")
        objectID = result.readString()
        if response - cmdID != 16 or retVarID != varID or objectID != objID:
            raise FatalTraCIError("Received answer %s,%s,%s for command %s,%s,%s."
                                  % (response, retVarID, objectID, cmdID, varID, objID))
        result.read("!B")     # Return type of the variable
        return result

    def _readSubscription(self, result):
        result.printDebug()  # to enable this you also need to set _DEBUG to True in storage.py
        result.readLength()
        response = result.read("!B")[0]
        isVariableSubscription = response >= tc.RESPONSE_SUBSCRIBE_INDUCTIONLOOP_VARIABLE and response <= tc.RESPONSE_SUBSCRIBE_PERSON_VARIABLE
        objectID = result.readString()
        if not isVariableSubscription:
            domain = result.read("!B")[0]
        numVars = result.read("!B")[0]
        if isVariableSubscription:
            while numVars > 0:
                varID = result.read("!B")[0]
                status, varType = result.read("!BB")
                if status:
                    print("Error!", result.readString())
                elif response in self._subscriptionMapping:
                    self._subscriptionMapping[response].add(objectID, varID, result)
                else:
                    raise FatalTraCIError(
                        "Cannot handle subscription response %02x for %s." % (response, objectID))
                numVars -= 1
        else:
            objectNo = result.read("!i")[0]
            for o in range(objectNo):
                oid = result.readString()
                if numVars == 0:
                    self._subscriptionMapping[response].addContext(
                        objectID, self._subscriptionMapping[domain], oid)
                for v in range(numVars):
                    varID = result.read("!B")[0]
                    status, varType = result.read("!BB")
                    if status:
                        print("Error!", result.readString())
                    elif response in self._subscriptionMapping:
                        self._subscriptionMapping[response].addContext(
                            objectID, self._subscriptionMapping[domain], oid, varID, result)
                    else:
                        raise FatalTraCIError(
                            "Cannot handle subscription response %02x for %s." % (response, objectID))
        return objectID, response

    def _subscribe(self, cmdID, begin, end, objID, varIDs, parameters=None):
        self._queue.append(cmdID)
        length = 1 + 1 + 4 + 4 + 4 + len(objID) + 1 + len(varIDs)
        if parameters:
            for v in varIDs:
                if v in parameters:
                    length += len(parameters[v])
        if length <= 255:
            self._string += struct.pack("!B", length)
        else:
            self._string += struct.pack("!Bi", 0, length + 4)
        self._string += struct.pack("!Biii",
                                       cmdID, begin, end, len(objID)) + objID.encode("latin1")
        self._string += struct.pack("!B", len(varIDs))
        for v in varIDs:
            self._string += struct.pack("!B", v)
            if parameters and v in parameters:
                self._string += parameters[v]
        result = self._sendExact()
        objectID, response = self._readSubscription(result)
        if response - cmdID != 16 or objectID != objID:
            raise FatalTraCIError("Received answer %02x,%s for subscription command %02x,%s." % (
                response, objectID, cmdID, objID))

    def _getSubscriptionResults(self, cmdID):
        return self._subscriptionMapping[cmdID]

    def _subscribeContext(self, cmdID, begin, end, objID, domain, dist, varIDs):
        self._queue.append(cmdID)
        length = 1 + 1 + 4 + 4 + 4 + len(objID) + 1 + 8 + 1 + len(varIDs)
        if length <= 255:
            self._string += struct.pack("!B", length)
        else:
            self._string += struct.pack("!Bi", 0, length + 4)
        self._string += struct.pack("!Biii",
                                       cmdID, begin, end, len(objID)) + objID.encode("latin1")
        self._string += struct.pack("!BdB", domain, dist, len(varIDs))
        for v in varIDs:
            self._string += struct.pack("!B", v)
        result = self._sendExact()
        objectID, response = self._readSubscription(result)
        if response - cmdID != 16 or objectID != objID:
            raise FatalTraCIError("Received answer %02x,%s for context subscription command %02x,%s." % (
                response, objectID, cmdID, objID))

    def isEmbedded(self):
        return _embedded

    def simulationStep(self, step=0):
        """
        Make a simulation step and simulate up to the given millisecond in sim time.
        If the given value is 0 or absent, exactly one step is performed.
        Values smaller than or equal to the current sim time result in no action.
        """
        self._queue.append(tc.CMD_SIMSTEP2)
        self._string += struct.pack("!BBi", 1 +
                                       1 + 4, tc.CMD_SIMSTEP2, step)
        result = self._sendExact()
        for subscriptionResults in self._subscriptionMapping.values():
            subscriptionResults.reset()
        numSubs = result.readInt()
        responses = []
        while numSubs > 0:
            responses.append(self._readSubscription(result))
            numSubs -= 1
        return responses

    def getVersion(self):
        command = tc.CMD_GETVERSION
        self._queue.append(command)
        self._string += struct.pack("!BB", 1 + 1, command)
        result = self._sendExact()
        result.readLength()
        response = result.read("!B")[0]
        if response != command:
            raise FatalTraCIError(
                "Received answer %s for command %s." % (response, command))
        return result.readInt(), result.readString()

    def close(self):
        if not _embedded:
            self._queue.append(tc.CMD_CLOSE)
            self._string += struct.pack("!BB", 1 + 1, tc.CMD_CLOSE)
            self._sendExact()
            self._socket.close()
            del self._socket