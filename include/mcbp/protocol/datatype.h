/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#pragma once

#include <cJSON_utils.h>
#include <cstdint>

namespace cb {
namespace mcbp {

/**
 * Definition of the data types in the packet
 * See section 3.4 Data Types
 */

enum class Datatype : uint8_t { Raw = 0, JSON = 1, Snappy = 2, Xattr = 4 };
} // namespace mcbp
} // namespace cb

std::string to_string(cb::mcbp::Datatype datatype);
unique_cJSON_ptr toJSON(cb::mcbp::Datatype datatype);
