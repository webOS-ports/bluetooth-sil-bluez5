// Copyright (c) 2014-2018 LG Electronics, Inc.
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

#ifndef ASYNCUTILS_H
#define ASYNCUTILS_H

#include <glib.h>
#include <gio/gio.h>
#include <functional>
#include <memory>

typedef std::function<void(GAsyncResult *result)> GlibAsyncFunction;
// With the object the call was made on: finishing a call on a proxy kept in a member, which by the time of the
// reply can be NULL, freed or another proxy, fails without setting an error, and was taken for success.
typedef std::function<void(GObject *source, GAsyncResult *result)> GlibAsyncSourceFunction;
typedef std::function<bool(void)> GlibSourceFunction;

class GlibAsyncFunctionWrapper
{
public:
	GlibAsyncFunctionWrapper(GlibAsyncFunction func) : mFunc(func) { }
	GlibAsyncFunctionWrapper(GlibAsyncSourceFunction func) : mSourceFunc(func) { }

	void call(GObject *source, GAsyncResult *result)
	{
		if (mSourceFunc)
			mSourceFunc(source, result);
		else
			mFunc(result);
	}

private:
	GlibAsyncFunction mFunc;
	GlibAsyncSourceFunction mSourceFunc;
};

// Owned by an object whose asynchronous calls can be answered after it is deleted: their callbacks capture
// token() and return at once when it has expired, instead of using the freed object.
class LifetimeGuard
{
public:
	LifetimeGuard() : mAlive(std::make_shared<char>(0)) { }
	LifetimeGuard(const LifetimeGuard &) = delete;
	LifetimeGuard &operator=(const LifetimeGuard &) = delete;

	std::weak_ptr<char> token() const { return mAlive; }

private:
	std::shared_ptr<char> mAlive;
};

class GlibSourceFunctionWrapper
{
public:
	GlibSourceFunctionWrapper(GlibSourceFunction func) : mFunc(func) { }

	gboolean call(void)
	{
		return (gboolean) mFunc();
	}

private:
	GlibSourceFunction mFunc;
};

void glibAsyncMethodWrapper(GObject *sourceObject, GAsyncResult *result, gpointer user_data);
gboolean glibSourceMethodWrapper(gpointer user_data);

#endif // ASYNCUTILS_H
