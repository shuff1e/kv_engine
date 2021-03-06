/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
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

#include "collections/manifest.h"
#include "bucket_logger.h"
#include "collections/collections_types.h"
#include "statwriter.h"
#include "utility.h"

#include <json_utilities.h>

#include <memcached/engine_error.h>
#include <nlohmann/json.hpp>
#include <platform/checked_snprintf.h>
#include <gsl/gsl>

#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>

namespace Collections {

// strings used in JSON parsing
static constexpr char const* ScopesKey = "scopes";
static constexpr nlohmann::json::value_t ScopesType =
        nlohmann::json::value_t::array;
static constexpr char const* CollectionsKey = "collections";
static constexpr nlohmann::json::value_t CollectionsType =
        nlohmann::json::value_t::array;
static constexpr char const* NameKey = "name";
static constexpr nlohmann::json::value_t NameType =
        nlohmann::json::value_t::string;
static constexpr char const* UidKey = "uid";
static constexpr nlohmann::json::value_t UidType =
        nlohmann::json::value_t::string;
static constexpr char const* MaxTtlKey = "max_ttl";
static constexpr nlohmann::json::value_t MaxTtlType =
        nlohmann::json::value_t::number_unsigned;

/**
 * Get json sub-object from the json object for key and check the type.
 * @param json The parent object in which to find key.
 * @param key The key to look for.
 * @param expectedType The type the found object must be.
 * @return A json object for key.
 * @throws std::invalid_argument if key is not found or the wrong type.
 */
nlohmann::json getJsonObject(const nlohmann::json& object,
                             const std::string& key,
                             nlohmann::json::value_t expectedType);

/**
 * Constructor helper function, throws invalid_argument with a string
 * indicating if the expectedType.
 *
 * @param errorKey the JSON key being looked up
 * @param object object to check
 * @param expectedType the type we expect object to be
 * @throws std::invalid_argument if !expectedType
 */
static void throwIfWrongType(const std::string& errorKey,
                             const nlohmann::json& object,
                             nlohmann::json::value_t expectedType);

Manifest::Manifest(cb::const_char_buffer json,
                   size_t maxNumberOfScopes,
                   size_t maxNumberOfCollections)
    : defaultCollectionExists(false), uid(0) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& e) {
        throw std::invalid_argument(
                "Manifest::Manifest nlohmann cannot parse json:" +
                cb::to_string(json) + ", e:" + e.what());
    }

    // Read the Manifest UID e.g. "uid" : "5fa1"
    auto jsonUid = getJsonObject(parsed, UidKey, UidType);
    uid = makeUid(jsonUid.get<std::string>());

    // Read the scopes within the Manifest
    auto scopes = getJsonObject(parsed, ScopesKey, ScopesType);

    // Check that we do not have too many before doing any parsing
    if (scopes.size() > maxNumberOfScopes) {
        throw std::invalid_argument(
                "Manifest::Manifest too many scopes count:" +
                std::to_string(scopes.size()));
    }

    for (const auto& scope : scopes) {
        throwIfWrongType(
                std::string(ScopesKey), scope, nlohmann::json::value_t::object);

        auto name = getJsonObject(scope, NameKey, NameType);
        auto uid = getJsonObject(scope, UidKey, UidType);

        auto nameValue = name.get<std::string>();
        if (!validName(nameValue)) {
            throw std::invalid_argument("Manifest::Manifest scope name: " +
                                        nameValue + " is not valid.");
        }

        // Construction of ScopeID checks for invalid values
        ScopeID uidValue = makeScopeID(uid.get<std::string>());

        // Scope uids must be unique
        if (this->scopes.count(uidValue) > 0) {
            throw std::invalid_argument(
                    "Manifest::Manifest duplicate scope uid:" +
                    uidValue.to_string() + ", name:" + nameValue);
        }

        // Scope names must be unique
        for (const auto& itr : this->scopes) {
            if (itr.second.name == nameValue) {
                throw std::invalid_argument(
                        "Manifest::Manifest duplicate scope name:" +
                        uidValue.to_string() + ", name:" + nameValue);
            }
        }

        std::vector<CollectionEntry> scopeCollections = {};

        // Read the collections within this scope
        auto collections =
                getJsonObject(scope, CollectionsKey, CollectionsType);

        // Check that the number of collections in this scope + the
        // number of already stored collections is not greater than the max
        if (collections.size() + this->collections.size() >
            maxNumberOfCollections) {
            throw std::invalid_argument(
                    "Manifest::Manifest too many collections count:" +
                    std::to_string(collections.size()));
        }

        for (const auto& collection : collections) {
            throwIfWrongType(std::string(CollectionsKey),
                             collection,
                             nlohmann::json::value_t::object);

            auto cname = getJsonObject(collection, NameKey, NameType);
            auto cuid = getJsonObject(collection, UidKey, UidType);
            auto cmaxttl = cb::getOptionalJsonObject(
                    collection, MaxTtlKey, MaxTtlType);

            auto cnameValue = cname.get<std::string>();
            if (!validName(cnameValue)) {
                throw std::invalid_argument(
                        "Manifest::Manifest collection name:" + cnameValue +
                        " is not valid");
            }

            // The constructor of CollectionID checks for invalid values, but
            // we need to check to ensure System (1) wasn't present in the
            // Manifest
            CollectionID cuidValue = makeCollectionID(cuid.get<std::string>());
            if (invalidCollectionID(cuidValue)) {
                throw std::invalid_argument(
                        "Manifest::Manifest collection uid: " +
                        cuidValue.to_string() + " is not valid.");
            }

            // Collection uids must be unique
            if (this->collections.count(cuidValue) > 0) {
                throw std::invalid_argument(
                        "Manifest::Manifest duplicate collection uid:" +
                        cuidValue.to_string() + ", name: " + cnameValue);
            }

            // Collection names must be unique within the scope
            for (const auto& itr : scopeCollections) {
                auto existingCollection = this->collections.find(itr.id);
                if (existingCollection != this->collections.end()) {
                    if (existingCollection->second == cnameValue) {
                        throw std::invalid_argument(
                                "Manifest::Manifest duplicate collection "
                                "name:" +
                                cuidValue.to_string() +
                                ", name: " + cnameValue);
                    }
                }
            }

            // The default collection must be within the default scope
            if (cuidValue.isDefaultCollection() && !uidValue.isDefaultScope()) {
                throw std::invalid_argument(
                        "Manifest::Manifest the default collection is"
                        " not in the default scope");
            }

            cb::ExpiryLimit maxTtl;
            if (cmaxttl) {
                // Don't exceed 32-bit max
                auto value = cmaxttl.get().get<uint64_t>();
                if (value > std::numeric_limits<uint32_t>::max()) {
                    throw std::out_of_range("Manifest::Manifest max_ttl:" +
                                            std::to_string(value));
                }
                maxTtl = std::chrono::seconds(value);
            }

            enableDefaultCollection(cuidValue);
            this->collections.emplace(cuidValue, cnameValue);
            scopeCollections.push_back({cuidValue, maxTtl});
        }

        this->scopes.emplace(uidValue,
                             Scope{nameValue, std::move(scopeCollections)});
    }

    if (this->scopes.empty()) {
        throw std::invalid_argument(
                "Manifest::Manifest no scopes were defined in the manifest");
    } else if (this->scopes.count(ScopeID::Default) == 0) {
        throw std::invalid_argument(
                "Manifest::Manifest the default scope was"
                " not defined");
    }
}

nlohmann::json getJsonObject(const nlohmann::json& object,
                             const std::string& key,
                             nlohmann::json::value_t expectedType) {
    return cb::getJsonObject(object, key, expectedType, "Manifest");
}

void throwIfWrongType(const std::string& errorKey,
                      const nlohmann::json& object,
                      nlohmann::json::value_t expectedType) {
    cb::throwIfWrongType(errorKey, object, expectedType, "Manifest");
}

void Manifest::enableDefaultCollection(CollectionID identifier) {
    if (identifier == CollectionID::Default) {
        defaultCollectionExists = true;
    }
}

bool Manifest::validName(const std::string& name) {
    // $ prefix is currently reserved for future use
    // Name cannot be empty
    if (name.empty() || name.size() > MaxCollectionNameSize || name[0] == '$') {
        return false;
    }
    // Check rest of the characters for validity
    for (const auto& c : name) {
        // Collection names are allowed to contain
        // A-Z, a-z, 0-9 and _ - % $
        // system collections are _ prefixed, but not enforced here
        if (!(std::isdigit(c) || std::isalpha(c) || c == '_' || c == '-' ||
              c == '%' || c == '$')) {
            return false;
        }
    }
    return true;
}

bool Manifest::invalidCollectionID(CollectionID identifier) {
    // System cannot appear in a manifest
    return identifier == CollectionID::System;
}

std::string Manifest::toJson() const {
    std::stringstream json;
    json << R"({"uid":")" << std::hex << uid << R"(","scopes":[)";
    size_t nScopes = 0;
    for (const auto& scope : scopes) {
        json << R"({"name":")" << scope.second.name << R"(","uid":")"
             << std::hex << scope.first << R"(")";
        // Add the collections if this scope has any
        if (!scope.second.collections.empty()) {
            json << R"(,"collections":[)";
            // Add all collections
            size_t nCollections = 0;
            for (const auto& collection : scope.second.collections) {
                json << R"({"name":")" << collections.at(collection.id)
                     << R"(","uid":")" << std::hex << collection.id << "\"";
                if (collection.maxTtl) {
                    json << R"(,"max_ttl":)" << std::dec
                         << collection.maxTtl.get().count();
                }
                json << "}";
                if (nCollections != scope.second.collections.size() - 1) {
                    json << ",";
                }
                nCollections++;
            }
            json << "]";
        } else {
            json << R"(,"collections":[])";
        }
        json << R"(})";
        if (nScopes != scopes.size() - 1) {
            json << ",";
        }
        nScopes++;
    }
    json << "]}";
    return json.str();
}

void Manifest::addCollectionStats(const void* cookie,
                                  const AddStatFn& add_stat) const {
    try {
        const int bsize = 512;
        char buffer[bsize];
        checked_snprintf(buffer, bsize, "manifest:collections");
        add_casted_stat(buffer, collections.size(), add_stat, cookie);
        checked_snprintf(buffer, bsize, "manifest:default_exists");
        add_casted_stat(buffer, defaultCollectionExists, add_stat, cookie);
        checked_snprintf(buffer, bsize, "manifest:uid");
        add_casted_stat(buffer, uid, add_stat, cookie);

        for (const auto& entry : collections) {
            checked_snprintf(buffer,
                             bsize,
                             "manifest:collection:%s:name",
                             entry.first.to_string().c_str());
            add_casted_stat(buffer, entry.second.c_str(), add_stat, cookie);
        }
    } catch (const std::exception& e) {
        EP_LOG_WARN(
                "Manifest::addCollectionStats failed to build stats "
                "exception:{}",
                e.what());
    }
}

void Manifest::addScopeStats(const void* cookie,
                             const AddStatFn& add_stat) const {
    try {
        const int bsize = 512;
        char buffer[bsize];
        checked_snprintf(buffer, bsize, "manifest:scopes");
        add_casted_stat(buffer, scopes.size(), add_stat, cookie);
        checked_snprintf(buffer, bsize, "manifest:uid");
        add_casted_stat(buffer, uid, add_stat, cookie);

        for (const auto& entry : scopes) {
            checked_snprintf(buffer,
                             bsize,
                             "manifest:scopes:%s:name",
                             entry.first.to_string().c_str());
            add_casted_stat(
                    buffer, entry.second.name.c_str(), add_stat, cookie);
            checked_snprintf(buffer,
                             bsize,
                             "manifest:scopes:%s:collections",
                             entry.first.to_string().c_str());
            add_casted_stat<unsigned long>(
                    buffer, entry.second.collections.size(), add_stat, cookie);
        }
    } catch (const std::exception& e) {
        EP_LOG_WARN(
                "Manifest::addScopeStats failed to build stats "
                "exception:{}",
                e.what());
    }
}

boost::optional<CollectionID> Manifest::getCollectionID(
        ScopeID scope, const std::string& path) const {
    int pos = path.find_first_of('.');
    std::string collection = path.substr(pos + 1);

    // Empty collection part of the path means default collection.
    if (collection.empty()) {
        collection = cb::to_string(DefaultCollectionIdentifier);
    }

    if (!validName(collection)) {
        throw cb::engine_error(
                cb::engine_errc::invalid_arguments,
                "Manifest::getCollectionID invalid collection:" + collection);
    }

    auto scopeItr = scopes.find(scope);
    if (scopeItr == scopes.end()) {
        throw cb::engine_error(
                cb::engine_errc::unknown_scope,
                "Manifest::getCollectionID unknown scope:" + scope.to_string());
    }
    for (const auto& c : scopeItr->second.collections) {
        auto cItr = collections.find(c.id);
        if (cItr != collections.end()) {
            if (cItr->second == collection) {
                return c.id;
            }
        }
    }

    return {};
}

boost::optional<ScopeID> Manifest::getScopeID(const std::string& path) const {
    int pos = path.find_first_of('.');
    std::string scope = path.substr(0, pos);

    // Empty scope part of the path means default scope.
    if (scope.empty()) {
        scope = cb::to_string(DefaultScopeIdentifier);
    }

    if (!(validName(scope))) {
        throw cb::engine_error(cb::engine_errc::invalid_arguments,
                               "Manifest::getScopeID invalid scope:" + scope);
    }

    for (const auto& s : scopes) {
        if (s.second.name == scope) {
            return s.first;
        }
    }

    return {};
}

void Manifest::dump() const {
    std::cerr << *this << std::endl;
}

std::ostream& operator<<(std::ostream& os, const Manifest& manifest) {
    os << "Collections::Manifest"
       << ", defaultCollectionExists:" << manifest.defaultCollectionExists
       << ", uid:" << manifest.uid
       << ", collections.size:" << manifest.collections.size() << std::endl;
    for (const auto& entry : manifest.scopes) {
        os << "scope:{" << std::hex << entry.first << ", " << entry.second.name
           << ", collections:[";
        for (const auto& collection : entry.second.collections) {
            os << "{" << std::hex << collection.id << ", "
               << manifest.collections.at(collection.id) << "}";
        }
        os << "]\n";
    }
    return os;
}
}
