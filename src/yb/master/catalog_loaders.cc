// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/master/catalog_loaders.h"

namespace yb {
namespace master {

////////////////////////////////////////////////////////////
// Table Loader
////////////////////////////////////////////////////////////

Status TableLoader::Visit(const TableId& table_id, const SysTablesEntryPB& metadata) {
  CHECK(!ContainsKey(catalog_manager_->table_ids_map_, table_id))
        << "Table already exists: " << table_id;

  // Setup the table info.
  scoped_refptr<TableInfo> table = catalog_manager_->NewTableInfo(table_id);
  auto l = table->LockForWrite();
  auto& pb = l->mutable_data()->pb;
  pb.CopyFrom(metadata);

  if (pb.table_type() == TableType::REDIS_TABLE_TYPE && pb.name() == kTransactionsTableName) {
    pb.set_table_type(TableType::TRANSACTION_STATUS_TABLE_TYPE);
  }

  // Add the table to the IDs map and to the name map (if the table is not deleted). Do not
  // add Postgres tables to the name map as the table name is not unique in a namespace.
  catalog_manager_->table_ids_map_[table->id()] = table;
  if (l->data().table_type() != PGSQL_TABLE_TYPE && !l->data().started_deleting()) {
    catalog_manager_->table_names_map_[{l->data().namespace_id(), l->data().name()}] = table;
  }

  l->Commit();
  catalog_manager_->HandleNewTableId(table->id());

  LOG(INFO) << "Loaded metadata for table " << table->ToString();
  VLOG(1) << "Metadata for table " << table->ToString() << ": " << metadata.ShortDebugString();

  return Status::OK();
}

////////////////////////////////////////////////////////////
// Tablet Loader
////////////////////////////////////////////////////////////

Status TabletLoader::Visit(const TabletId& tablet_id, const SysTabletsEntryPB& metadata) {
  // Lookup the table.
  scoped_refptr<TableInfo> first_table(FindPtrOrNull(
                                    catalog_manager_->table_ids_map_, metadata.table_id()));

  // Setup the tablet info.
  TabletInfo* tablet = new TabletInfo(first_table, tablet_id);
  auto l = tablet->LockForWrite();
  l->mutable_data()->pb.CopyFrom(metadata);

  // Add the tablet to the tablet manager.
  auto inserted = catalog_manager_->tablet_map_.emplace(tablet->tablet_id(), tablet).second;
  if (!inserted) {
    return STATUS_FORMAT(
        IllegalState, "Loaded tablet that already in map: $0", tablet->tablet_id());
  }

  std::vector<TableId> table_ids;
  for (int k = 0; k < metadata.table_ids_size(); ++k) {
    table_ids.push_back(metadata.table_ids(k));
  }

  // This is for backwards compatibility: we want to ensure that the table_ids
  // list contains the first table that created the tablet. If the table_ids field
  // was empty, we "upgrade" the master to support this new invariant.
  if (metadata.table_ids_size() == 0) {
    l->mutable_data()->pb.add_table_ids(metadata.table_id());
    Status s = catalog_manager_->sys_catalog_->UpdateItem(
        tablet, catalog_manager_->leader_ready_term_);
    if (PREDICT_FALSE(!s.ok())) {
      return STATUS_FORMAT(
          IllegalState, "An error occurred while inserting to sys-tablets: $0", s);
    }
    table_ids.push_back(metadata.table_id());
  }

  for (auto table_id : table_ids) {
    scoped_refptr<TableInfo> table(FindPtrOrNull(catalog_manager_->table_ids_map_, table_id));

    if (table == nullptr) {
      // If the table is missing and the tablet is in "preparing" state
      // may mean that the table was not created (maybe due to a failed write
      // for the sys-tablets). The cleaner will remove.
      if (l->data().pb.state() == SysTabletsEntryPB::PREPARING) {
        LOG(WARNING) << "Missing table " << table_id << " required by tablet " << tablet_id
                      << " (probably a failed table creation: the tablet was not assigned)";
        return Status::OK();
      }

      // if the tablet is not in a "preparing" state, something is wrong...
      LOG(ERROR) << "Missing table " << table_id << " required by tablet " << tablet_id
                  << ", metadata: " << metadata.DebugString()
                  << ", tables: " << yb::ToString(catalog_manager_->table_ids_map_);
      return STATUS(Corruption, "Missing table for tablet: ", tablet_id);
    }

    // Add the tablet to the Table.
    if (!l->mutable_data()->is_deleted()) {
      table->AddTablet(tablet);
    }
  }

  l->Commit();

  // TODO(KUDU-1070): if we see a running tablet under a deleted table,
  // we should "roll forward" the deletion of the tablet here.

  LOG(INFO) << "Loaded metadata for tablet " << tablet_id
            << " (first table " << first_table->ToString() << ")";
  VLOG(1) << "Metadata for tablet " << tablet_id << ": " << metadata.ShortDebugString();

  return Status::OK();
}

////////////////////////////////////////////////////////////
// Namespace Loader
////////////////////////////////////////////////////////////

Status NamespaceLoader::Visit(const NamespaceId& ns_id, const SysNamespaceEntryPB& metadata) {
  CHECK(!ContainsKey(catalog_manager_->namespace_ids_map_, ns_id))
    << "Namespace already exists: " << ns_id;

  // Setup the namespace info.
  NamespaceInfo *const ns = new NamespaceInfo(ns_id);
  auto l = ns->LockForWrite();
  l->mutable_data()->pb.CopyFrom(metadata);

  if (!l->data().pb.has_database_type() ||
      l->data().pb.database_type() == YQL_DATABASE_UNKNOWN) {
    LOG(INFO) << "Updating database type of namespace " << l->data().pb.name();
    l->mutable_data()->pb.set_database_type(l->data().pb.name() == common::kRedisKeyspaceName
                                            ? YQLDatabase::YQL_DATABASE_REDIS
                                            : YQLDatabase::YQL_DATABASE_CQL);
  }

  // Add the namespace to the IDs map and to the name map (if the namespace is not deleted).
  // Do not add Postgres namespace to the name map as it will be identified by id only.
  catalog_manager_->namespace_ids_map_[ns_id] = ns;
  if (!l->data().pb.name().empty() && l->data().pb.database_type() != YQL_DATABASE_PGSQL) {
    catalog_manager_->namespace_names_map_[l->data().pb.name()] = ns;
  }

  l->Commit();

  LOG(INFO) << "Loaded metadata for namespace " << ns->ToString();
  VLOG(1) << "Metadata for namespace " << ns->ToString() << ": " << metadata.ShortDebugString();

  return Status::OK();
}

////////////////////////////////////////////////////////////
// User-Defined Type Loader
////////////////////////////////////////////////////////////

Status UDTypeLoader::Visit(const UDTypeId& udtype_id, const SysUDTypeEntryPB& metadata) {
  CHECK(!ContainsKey(catalog_manager_->udtype_ids_map_, udtype_id))
      << "Type already exists: " << udtype_id;

  // Setup the table info.
  UDTypeInfo *const udtype = new UDTypeInfo(udtype_id);
  auto l = udtype->LockForWrite();
  l->mutable_data()->pb.CopyFrom(metadata);

  // Add the used-defined type to the IDs map and to the name map (if the type is not deleted).
  catalog_manager_->udtype_ids_map_[udtype->id()] = udtype;
  if (!l->data().name().empty()) { // If name is set (non-empty) then type is not deleted.
    catalog_manager_->udtype_names_map_[{l->data().namespace_id(), l->data().name()}] = udtype;
  }

  l->Commit();

  LOG(INFO) << "Loaded metadata for type " << udtype->ToString();
  VLOG(1) << "Metadata for type " << udtype->ToString() << ": " << metadata.ShortDebugString();

  return Status::OK();
}

////////////////////////////////////////////////////////////
// Config Loader
////////////////////////////////////////////////////////////

Status ClusterConfigLoader::Visit(
    const std::string& unused_id, const SysClusterConfigEntryPB& metadata) {
  // Debug confirm that there is no cluster_config_ set. This also ensures that this does not
  // visit multiple rows. Should update this, if we decide to have multiple IDs set as well.
  DCHECK(!catalog_manager_->cluster_config_) << "Already have config data!";

  // Prepare the config object.
  ClusterConfigInfo* config = new ClusterConfigInfo();
  auto l = config->LockForWrite();
  l->mutable_data()->pb.CopyFrom(metadata);

  if (metadata.has_server_blacklist()) {
    // Rebuild the blacklist state for load movement completion tracking.
    RETURN_NOT_OK(catalog_manager_->SetBlackList(metadata.server_blacklist()));
  }

  if (metadata.has_leader_blacklist()) {
    // Rebuild the blacklist state for load movement completion tracking.
    RETURN_NOT_OK(catalog_manager_->SetLeaderBlacklist(metadata.leader_blacklist()));
  }

  // Update in memory state.
  catalog_manager_->cluster_config_ = config;
  l->Commit();

  return Status::OK();
}

////////////////////////////////////////////////////////////
// Redis Config Loader
////////////////////////////////////////////////////////////

Status RedisConfigLoader::Visit(const std::string& key, const SysRedisConfigEntryPB& metadata) {
  CHECK(!ContainsKey(catalog_manager_->redis_config_map_, key))
      << "Redis Config with key already exists: " << key;
  // Prepare the config object.
  RedisConfigInfo* config = new RedisConfigInfo(key);
  auto l = config->LockForWrite();
  l->mutable_data()->pb.CopyFrom(metadata);
  catalog_manager_->redis_config_map_[key] = config;
  l->Commit();
  return Status::OK();
}

////////////////////////////////////////////////////////////
// Role Loader
////////////////////////////////////////////////////////////

Status RoleLoader::Visit(const RoleName& role_name, const SysRoleEntryPB& metadata) {
  CHECK(!catalog_manager_->permissions_manager()->DoesRoleExistUnlocked(role_name))
    << "Role already exists: " << role_name;

  RoleInfo* const role = new RoleInfo(role_name);
  auto l = role->LockForWrite();
  l->mutable_data()->pb.CopyFrom(metadata);
  catalog_manager_->permissions_manager()->AddRoleUnlocked(
      role_name, make_scoped_refptr<RoleInfo>(role));

  l->Commit();

  LOG(INFO) << "Loaded metadata for role " << role->id();
  VLOG(1) << "Metadata for role " << role->id() << ": " << metadata.ShortDebugString();

  return Status::OK();
}

////////////////////////////////////////////////////////////
// Sys Config Loader
////////////////////////////////////////////////////////////

Status SysConfigLoader::Visit(const string& config_type, const SysConfigEntryPB& metadata) {
  SysConfigInfo* const config = new SysConfigInfo(config_type);
  auto l = config->LockForWrite();
  l->mutable_data()->pb.CopyFrom(metadata);

  // For now we are only using this to store (ycql) security config or ysql catalog config.
  if (config_type == kSecurityConfigType) {
    catalog_manager_->permissions_manager()->SetSecurityConfigOnLoadUnlocked(config);
  } else if (config_type == kYsqlCatalogConfigType) {
    LOG_IF(WARNING, catalog_manager_->ysql_catalog_config_ != nullptr)
        << "Multiple sys config type " << config_type << " found";
    catalog_manager_->ysql_catalog_config_ = config;
  }

  l->Commit();

  LOG(INFO) << "Loaded sys config type " << config_type;
  return Status::OK();
}

}  // namespace master
}  // namespace yb
