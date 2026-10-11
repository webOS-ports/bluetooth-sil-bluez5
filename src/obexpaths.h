// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

// Names that come from obexd and from the remote device: OBEX object paths, MAP message handles, and the name a
// pushed file is stored under. Kept apart so that they can be tested on their own.

#ifndef OBEXPATHS_H
#define OBEXPATHS_H

#include <cstdint>
#include <functional>
#include <string>

// The "sessionN" element of an obexd object path (/org/bluez/obex/client/session12/message3 -> session12), or "".
std::string obexSessionId(const std::string &objectPath);

// The "messageN" element of a MAP message path, or "".
std::string obexMessageHandle(const std::string &objectPath);

// One element of a D-Bus object path: [A-Za-z0-9_]+. A MAP message handle from an app goes into a path.
bool isObjectPathElement(const std::string &element);

// A session key with its address lower-cased: a plain address, or MAP's "<address>_<instance>" whose instance name
// keeps its case. obexd reports addresses upper-case, the service passes them lower-case.
std::string normaliseSessionKey(const std::string &sessionKey);

// Whether a pushed file may be stored under its own name: not empty, no '/', not starting with '.', no control
// characters.
bool isAcceptableReceivedName(const std::string &name);

// The name to store a pushed file under, in a folder where exists(name) tells what is taken: the name itself, or
// "stem (n).ext" for the first n from 1 to 999 that is free. "" when the name is not acceptable or all are taken.
std::string uniqueReceivedName(const std::string &name, const std::function<bool(const std::string &)> &exists);

// Whether path is what is left of a push that failed: a regular file (not a link) shorter than announcedSize, the
// size the sender gave. A push without a size (0) is never taken as partial, nor a file that is complete.
bool isPartOfFailedPush(const std::string &path, uint64_t announcedSize);

#endif
