/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "conn_mgr.h"
#include "common.h"
#include "action_helpers.h"

#include <PI/pi.h>
#include <PI/p4info.h>
#include <PI/int/pi_int.h>
#include <PI/int/serialize.h>

#include <string>
#include <vector>
#include <unordered_map>

#include <cstring>

namespace pibmv2 {

extern conn_mgr_t *conn_mgr_state;

}  // namespace pibmv2

namespace {

std::vector<BmMatchParam> build_key(pi_p4_id_t table_id,
                                    const pi_match_key_t *match_key,
                                    const pi_p4info_t *p4info,
                                    bool *requires_priority) {
  std::vector<BmMatchParam> key;
  *requires_priority = false;

  const char *mk_data = match_key->data;

  BmMatchParam param;
  BmMatchParamValid param_valid;
  BmMatchParamExact param_exact;
  BmMatchParamLPM param_lpm;
  BmMatchParamTernary param_ternary;
  BmMatchParamRange param_range;

  size_t num_match_fields = pi_p4info_table_num_match_fields(p4info, table_id);
  for (size_t i = 0; i < num_match_fields; i++) {
    pi_p4info_match_field_info_t finfo;
    pi_p4info_table_match_field_info(p4info, table_id, i, &finfo);
    size_t f_bw = finfo.bitwidth;
    size_t nbytes = (f_bw + 7) / 8;
    uint32_t pLen;

    switch (finfo.match_type) {
      case PI_P4INFO_MATCH_TYPE_VALID:
        param_valid.key = (*mk_data != 0);
        mk_data++;
        param = BmMatchParam();
        param.type = BmMatchParamType::type::VALID;
        param.__set_valid(param_valid);  // does a copy of param_valid
        key.push_back(std::move(param));
        break;
      case PI_P4INFO_MATCH_TYPE_EXACT:
        param_exact.key = std::string(mk_data, nbytes);
        mk_data += nbytes;
        param = BmMatchParam();
        param.type = BmMatchParamType::type::EXACT;
        param.__set_exact(param_exact);  // does a copy of param_exact
        key.push_back(std::move(param));
        break;
      case PI_P4INFO_MATCH_TYPE_LPM:
        param_lpm.key = std::string(mk_data, nbytes);
        mk_data += nbytes;
        mk_data += retrieve_uint32(mk_data, &pLen);
        param_lpm.prefix_length = static_cast<int32_t>(pLen);
        param = BmMatchParam();
        param.type = BmMatchParamType::type::LPM;
        param.__set_lpm(param_lpm);  // does a copy of param_lpm
        key.push_back(std::move(param));
        break;
      case PI_P4INFO_MATCH_TYPE_TERNARY:
        param_ternary.key = std::string(mk_data, nbytes);
        mk_data += nbytes;
        param_ternary.mask = std::string(mk_data, nbytes);
        mk_data += nbytes;
        param = BmMatchParam();
        param.type = BmMatchParamType::type::TERNARY;
        param.__set_ternary(param_ternary);  // does a copy of param_ternary
        key.push_back(std::move(param));

        *requires_priority = true;
        break;
      case PI_P4INFO_MATCH_TYPE_RANGE:
        param_range.start = std::string(mk_data, nbytes);
        mk_data += nbytes;
        param_range.end_ = std::string(mk_data, nbytes);
        mk_data += nbytes;
        param = BmMatchParam();
        param.type = BmMatchParamType::type::RANGE;
        param.__set_range(param_range);  // does a copy of param_range
        key.push_back(std::move(param));

        *requires_priority = true;
        break;
      default:
        assert(0);
    }
  }

  return key;
}

char *dump_action_data(const pi_p4info_t *p4info, char *data,
                       pi_p4_id_t action_id, const BmActionData &params) {
  // unfortunately, I have observed that bmv2 sometimes returns shorter binary
  // strings than it received (0 padding is removed), which makes things more
  // complicated and expensive here.
  size_t num_params;
  const pi_p4_id_t *param_ids = pi_p4info_action_get_params(
      p4info, action_id, &num_params);
  assert(num_params == params.size());
  for (size_t i = 0; i < num_params; i++) {
    size_t bitwidth = pi_p4info_action_param_bitwidth(p4info, param_ids[i]);
    size_t nbytes = (bitwidth + 7) / 8;
    const auto &p = params.at(i);
    assert(nbytes >= p.size());
    size_t diff = nbytes - p.size();
    std::memset(data, 0, diff);
    std::memcpy(data + diff, p.data(), p.size());
    data += nbytes;
  }
  return data;
}


pi_entry_handle_t add_entry(const pi_p4info_t *p4info,
                            pi_dev_tgt_t dev_tgt,
                            const std::string t_name,
                            const BmMatchParams &mkey,
                            const pi_action_data_t *adata,
                            const BmAddEntryOptions &options) {
  auto action_data = pibmv2::build_action_data(adata, p4info);
  pi_p4_id_t action_id = adata->action_id;
  std::string a_name(pi_p4info_action_name_from_id(p4info, action_id));

  auto client = conn_mgr_client(pibmv2::conn_mgr_state, dev_tgt.dev_id);

  return client.c->bm_mt_add_entry(
      0, t_name, mkey, a_name, action_data, options);
}

pi_entry_handle_t add_indirect_entry(const pi_p4info_t *p4info,
                                     pi_dev_tgt_t dev_tgt,
                                     const std::string t_name,
                                     const BmMatchParams &mkey,
                                     pi_indirect_handle_t h,
                                     const BmAddEntryOptions &options) {
  (void) p4info;  // needed later?
  auto client = conn_mgr_client(pibmv2::conn_mgr_state, dev_tgt.dev_id);

  bool is_grp_h = pibmv2::IndirectHMgr::is_grp_h(h);
  if (!is_grp_h) {
    return client.c->bm_mt_indirect_add_entry(
        0, t_name, mkey, h, options);
  } else {
    h = pibmv2::IndirectHMgr::clear_grp_h(h);
    return client.c->bm_mt_indirect_ws_add_entry(
        0, t_name, mkey, h, options);
  }
}

}  // namespace


extern "C" {

pi_status_t _pi_table_entry_add(pi_session_handle_t session_handle,
                                pi_dev_tgt_t dev_tgt,
                                pi_p4_id_t table_id,
                                const pi_match_key_t *match_key,
                                const pi_table_entry_t *table_entry,
                                int overwrite,
                                pi_entry_handle_t *entry_handle) {
  (void) overwrite;  // TODO
  (void) session_handle;

  pibmv2::device_info_t *d_info = pibmv2::get_device_info(dev_tgt.dev_id);
  assert(d_info->assigned);
  const pi_p4info_t *p4info = d_info->p4info;

  bool requires_priority = false;
  std::vector<BmMatchParam> mkey = build_key(table_id, match_key, p4info,
                                             &requires_priority);

  BmAddEntryOptions options;
  if (requires_priority) {
    int priority = 0;
    const pi_entry_properties_t *properties = table_entry->entry_properties;
    if (properties &&
        pi_entry_properties_is_set(properties, PI_ENTRY_PROPERTY_TYPE_PRIORITY))
      priority = static_cast<int>(properties->priority);
    // TODO(antonin): if no priority found we set the value to 0, we should
    // probably find a better way
    options.__set_priority(static_cast<int32_t>(priority));
  }

  std::string t_name(pi_p4info_table_name_from_id(p4info, table_id));

  // TODO: entry timeout
  // TODO: direct meters
  try {
    switch (table_entry->entry_type) {
      case PI_ACTION_ENTRY_TYPE_DATA:
        *entry_handle = add_entry(p4info, dev_tgt, t_name, mkey,
                                  table_entry->entry.action_data, options);
        break;
      case PI_ACTION_ENTRY_TYPE_INDIRECT:
        *entry_handle = add_indirect_entry(p4info, dev_tgt, t_name, mkey,
                                           table_entry->entry.indirect_handle,
                                           options);
        break;
      default:
        assert(0);
    }
  } catch (InvalidTableOperation &ito) {
    const char *what =
        _TableOperationErrorCode_VALUES_TO_NAMES.find(ito.code)->second;
    std::cout << "Invalid table (" << t_name << ") operation ("
              << ito.code << "): " << what << std::endl;
    return static_cast<pi_status_t>(PI_STATUS_TARGET_ERROR + ito.code);
  }

  return PI_STATUS_SUCCESS;
}

pi_status_t _pi_table_default_action_set(pi_session_handle_t session_handle,
                                         pi_dev_tgt_t dev_tgt,
                                         pi_p4_id_t table_id,
                                         const pi_table_entry_t *table_entry) {
  (void) session_handle;

  pibmv2::device_info_t *d_info = pibmv2::get_device_info(dev_tgt.dev_id);
  assert(d_info->assigned);
  const pi_p4info_t *p4info = d_info->p4info;

  pi_p4_id_t action_id = table_entry->entry.action_data->action_id;
  if (pi_p4info_table_has_const_default_action(p4info, table_id)) {
    const pi_p4_id_t default_action_id =
        pi_p4info_table_get_const_default_action(p4info, table_id);
    if (default_action_id != action_id)
      return PI_STATUS_CONST_DEFAULT_ACTION;
  }

  auto action_data = pibmv2::build_action_data(table_entry->entry.action_data,
                                               p4info);

  std::string t_name(pi_p4info_table_name_from_id(p4info, table_id));
  std::string a_name(pi_p4info_action_name_from_id(p4info, action_id));

  auto client = conn_mgr_client(pibmv2::conn_mgr_state, dev_tgt.dev_id);

  try {
    client.c->bm_mt_set_default_action(0, t_name, a_name, action_data);
  } catch (InvalidTableOperation &ito) {
    const char *what =
        _TableOperationErrorCode_VALUES_TO_NAMES.find(ito.code)->second;
    std::cout << "Invalid table (" << t_name << ") operation ("
              << ito.code << "): " << what << std::endl;
    return static_cast<pi_status_t>(PI_STATUS_TARGET_ERROR + ito.code);
  }

  return PI_STATUS_SUCCESS;
}

pi_status_t _pi_table_default_action_get(pi_session_handle_t session_handle,
                                         pi_dev_id_t dev_id,
                                         pi_p4_id_t table_id,
                                         pi_table_entry_t *table_entry) {
  (void) session_handle;

  pibmv2::device_info_t *d_info = pibmv2::get_device_info(dev_id);
  assert(d_info->assigned);
  const pi_p4info_t *p4info = d_info->p4info;

  std::string t_name(pi_p4info_table_name_from_id(p4info, table_id));

  BmActionEntry entry;
  try {
    conn_mgr_client(pibmv2::conn_mgr_state, dev_id).c->bm_mt_get_default_entry(
        entry, 0, t_name);
  } catch (InvalidTableOperation &ito) {
    const char *what =
        _TableOperationErrorCode_VALUES_TO_NAMES.find(ito.code)->second;
    std::cout << "Invalid table (" << t_name << ") operation ("
              << ito.code << "): " << what << std::endl;
    return static_cast<pi_status_t>(PI_STATUS_TARGET_ERROR + ito.code);
  }

  if (entry.action_type == BmActionEntryType::NONE) {
    table_entry->entry_type = PI_ACTION_ENTRY_TYPE_NONE;
    // should we return an error code?
    return PI_STATUS_SUCCESS;
  }

  assert(entry.action_type == BmActionEntryType::ACTION_DATA);
  const pi_p4_id_t action_id = pi_p4info_action_id_from_name(
      p4info, entry.action_name.c_str());

  // TODO(antonin): indirect
  table_entry->entry_type = PI_ACTION_ENTRY_TYPE_DATA;

  const size_t adata_size = get_action_data_size(p4info, action_id);

  // no alignment issue with new[]
  char *data_ = new char[sizeof(pi_action_data_t) + adata_size];
  pi_action_data_t *action_data = reinterpret_cast<pi_action_data_t *>(data_);
  data_ += sizeof(pi_action_data_t);

  action_data->p4info = p4info;
  action_data->action_id = action_id;
  action_data->data_size = adata_size;
  action_data->data = data_;

  table_entry->entry.action_data = action_data;

  data_ = dump_action_data(p4info, data_, action_id, entry.action_data);

  return PI_STATUS_SUCCESS;
}

pi_status_t _pi_table_default_action_done(pi_session_handle_t session_handle,
                                          pi_table_entry_t *table_entry) {
  (void) session_handle;

  if (table_entry->entry_type == PI_ACTION_ENTRY_TYPE_DATA) {
    pi_action_data_t *action_data = table_entry->entry.action_data;
    if (action_data) delete[] action_data;
  }

  return PI_STATUS_SUCCESS;
}

pi_status_t _pi_table_entry_delete(pi_session_handle_t session_handle,
                                   pi_dev_id_t dev_id,
                                   pi_p4_id_t table_id,
                                   pi_entry_handle_t entry_handle) {
  (void) session_handle;

  pibmv2::device_info_t *d_info = pibmv2::get_device_info(dev_id);
  assert(d_info->assigned);
  const pi_p4info_t *p4info = d_info->p4info;

  std::string t_name(pi_p4info_table_name_from_id(p4info, table_id));

  auto client = conn_mgr_client(pibmv2::conn_mgr_state, dev_id);

  try {
    client.c->bm_mt_delete_entry(0, t_name, entry_handle);
  } catch (InvalidTableOperation &ito) {
    const char *what =
        _TableOperationErrorCode_VALUES_TO_NAMES.find(ito.code)->second;
    std::cout << "Invalid table (" << t_name << ") operation ("
              << ito.code << "): " << what << std::endl;
    return static_cast<pi_status_t>(PI_STATUS_TARGET_ERROR + ito.code);
  }

  return PI_STATUS_SUCCESS;
}

pi_status_t _pi_table_entry_modify(pi_session_handle_t session_handle,
                                   pi_dev_id_t dev_id,
                                   pi_p4_id_t table_id,
                                   pi_entry_handle_t entry_handle,
                                   const pi_table_entry_t *table_entry) {
  (void) session_handle;

  pibmv2::device_info_t *d_info = pibmv2::get_device_info(dev_id);
  assert(d_info->assigned);
  const pi_p4info_t *p4info = d_info->p4info;

  auto action_data = pibmv2::build_action_data(table_entry->entry.action_data,
                                               p4info);

  std::string t_name(pi_p4info_table_name_from_id(p4info, table_id));
  pi_p4_id_t action_id = table_entry->entry.action_data->action_id;
  std::string a_name(pi_p4info_action_name_from_id(p4info, action_id));

  auto client = conn_mgr_client(pibmv2::conn_mgr_state, dev_id);

  try {
    client.c->bm_mt_modify_entry(0, t_name, entry_handle, a_name, action_data);
  } catch (InvalidTableOperation &ito) {
    const char *what =
        _TableOperationErrorCode_VALUES_TO_NAMES.find(ito.code)->second;
    std::cout << "Invalid table (" << t_name << ") operation ("
              << ito.code << "): " << what << std::endl;
    return static_cast<pi_status_t>(PI_STATUS_TARGET_ERROR + ito.code);
  }

  return PI_STATUS_SUCCESS;
}

pi_status_t _pi_table_entries_fetch(pi_session_handle_t session_handle,
                                    pi_dev_id_t dev_id,
                                    pi_p4_id_t table_id,
                                    pi_table_fetch_res_t *res) {
  (void) session_handle;

  pibmv2::device_info_t *d_info = pibmv2::get_device_info(dev_id);
  assert(d_info->assigned);
  const pi_p4info_t *p4info = d_info->p4info;

  std::string t_name(pi_p4info_table_name_from_id(p4info, table_id));

  std::vector<BmMtEntry> entries;
  try {
    conn_mgr_client(pibmv2::conn_mgr_state, dev_id).c->bm_mt_get_entries(
        entries, 0, t_name);
  } catch (InvalidTableOperation &ito) {
    const char *what =
        _TableOperationErrorCode_VALUES_TO_NAMES.find(ito.code)->second;
    std::cout << "Invalid table (" << t_name << ") operation ("
              << ito.code << "): " << what << std::endl;
    return static_cast<pi_status_t>(PI_STATUS_TARGET_ERROR + ito.code);
  }

  res->num_entries = entries.size();

  size_t data_size = 0u;

  data_size += entries.size() * sizeof(uint64_t);  // entry handles
  data_size += entries.size() * sizeof(uint32_t);  // action ids
  data_size += entries.size() * sizeof(uint32_t);  // action data nbytes
  // TODO(antonin): make this conditional on the table match type.
  // allocating too much memory is not an issue though
  data_size += entries.size() * 2 * sizeof(uint32_t);  // for priority

  res->mkey_nbytes = get_match_key_size(p4info, table_id);
  data_size += entries.size() * res->mkey_nbytes;

  struct ADataSize {
    ADataSize(pi_p4_id_t id, size_t s)
        : id(id), s(s) { }
    pi_p4_id_t id;
    size_t s;
  };

  size_t num_actions;
  const pi_p4_id_t *action_ids = pi_p4info_table_get_actions(p4info, table_id,
                                                             &num_actions);
  std::unordered_map<std::string, ADataSize> action_map;
  action_map.reserve(num_actions);

  for (size_t i = 0; i < num_actions; i++) {
    action_map.emplace(
        std::string(pi_p4info_action_name_from_id(p4info, action_ids[i])),
        ADataSize(action_ids[i], get_action_data_size(p4info, action_ids[i])));
  }

  for (const auto &e : entries) {
    data_size += action_map.at(e.action_entry.action_name).s;
  }

  char *data = new char[data_size];
  res->entries_size = data_size;
  res->entries = data;

  for (auto &e : entries) {
    data += emit_entry_handle(data, e.entry_handle);
    for (auto p : e.match_key) {
      switch(p.type) {
        case BmMatchParamType::type::EXACT:
          std::memcpy(data, p.exact.key.data(), p.exact.key.size());
          data += p.exact.key.size();
          break;
        case BmMatchParamType::type::LPM:
          std::memcpy(data, p.lpm.key.data(), p.lpm.key.size());
          data += p.lpm.key.size();
          data += emit_uint32(data, p.lpm.prefix_length);
          break;
        case BmMatchParamType::type::TERNARY:
          std::memcpy(data, p.ternary.key.data(), p.ternary.key.size());
          data += p.ternary.key.size();
          std::memcpy(data, p.ternary.mask.data(), p.ternary.mask.size());
          data += p.ternary.mask.size();
          break;
        case BmMatchParamType::type::VALID:
          *data = p.valid.key;
          data++;
          break;
        case BmMatchParamType::type::RANGE:
          std::memcpy(data, p.range.start.data(), p.range.start.size());
          data += p.range.start.size();
          std::memcpy(data, p.range.end_.data(), p.range.end_.size());
          data += p.range.end_.size();
          break;
      }
    }

    const BmActionEntry &action_entry = e.action_entry;
    assert(action_entry.action_type == BmActionEntryType::ACTION_DATA);
    const ADataSize &adata_size = action_map.at(action_entry.action_name);
    data += emit_p4_id(data, adata_size.id);
    data += emit_uint32(data, adata_size.s);

    data = dump_action_data(p4info, data, adata_size.id,
                            action_entry.action_data);

    const BmAddEntryOptions &options = e.options;
    if (options.__isset.priority) {
      data += emit_uint32(data, 1 << PI_ENTRY_PROPERTY_TYPE_PRIORITY);
      data += emit_uint32(data, options.priority);
    } else {
      data += emit_uint32(data, 0);
    }
  }

  return PI_STATUS_SUCCESS;
}

pi_status_t _pi_table_entries_fetch_done(pi_session_handle_t session_handle,
                                         pi_table_fetch_res_t *res) {
  (void) session_handle;

  delete[] res->entries;
  return PI_STATUS_SUCCESS;
}

}
