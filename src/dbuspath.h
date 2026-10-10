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

// D-Bus object path relations, kept apart so that they can be tested.

#ifndef DBUSPATH_H
#define DBUSPATH_H

#include <string>

// path is parent itself or lies below it: "/org/bluez/hci1/dev_X" is under "/org/bluez/hci1", but
// "/org/bluez/hci10/dev_X" is not.
bool isObjectPathUnder(const std::string &path, const std::string &parent);

#endif
