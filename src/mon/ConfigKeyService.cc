// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 Inktank, Inc
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <sstream>
#include <stdlib.h>
#include <limits.h>

#include "mon/Monitor.h"
#include "mon/ConfigKeyService.h"
#include "mon/MonitorDBStore.h"
#include "mon/OSDMonitor.h"
#include "common/errno.h"
#include "include/stringify.h"

#include "include/ceph_assert.h" // re-clobber ceph_assert()
#define dout_subsys ceph_subsys_mon
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, this)
using namespace TOPNSPC::common;

using namespace std::literals;
using std::cerr;
using std::cout;
using std::dec;
using std::hex;
using std::list;
using std::map;
using std::make_pair;
using std::ostream;
using std::ostringstream;
using std::pair;
using std::set;
using std::setfill;
using std::string;
using std::stringstream;
using std::to_string;
using std::vector;
using std::unique_ptr;

using ceph::bufferlist;
using ceph::decode;
using ceph::encode;
using ceph::Formatter;
using ceph::JSONFormatter;
using ceph::mono_clock;
using ceph::mono_time;
using ceph::parse_timespan;
using ceph::timespan_str;

static ostream& _prefix(std::ostream *_dout, const Monitor &mon,
                        const ConfigKeyService *service) {
  return *_dout << "mon." << mon.name << "@" << mon.rank
		<< "(" << mon.get_state_name() << ")." << service->get_name()
                << "(" << service->get_epoch() << ") ";
}

const string CONFIG_PREFIX = "mon_config_key";

ConfigKeyService::ConfigKeyService(Monitor &m, Paxos &p)
  : mon(m),
    paxos(p),
    tick_period(g_conf()->mon_tick_interval)
{}

void ConfigKeyService::start(epoch_t new_epoch)
{
  epoch = new_epoch;
  start_epoch();
}

void ConfigKeyService::finish()
{
  generic_dout(20) << "ConfigKeyService::finish" << dendl;
  finish_epoch();
}

epoch_t ConfigKeyService::get_epoch() const {
  return epoch;
}

bool ConfigKeyService::dispatch(MonOpRequestRef op) {
  return service_dispatch(op);
}

bool ConfigKeyService::in_quorum() const
{
  return (mon.is_leader() || mon.is_peon());
}

void ConfigKeyService::start_tick()
{
  generic_dout(10) << __func__ << dendl;

  cancel_tick();
  if (tick_period <= 0)
    return;

  tick_event = new C_MonContext{&mon, [this](int r) {
    if (r < 0) {
      return;
    }
    tick();
  }};
  mon.timer.add_event_after(tick_period, tick_event);
}

void ConfigKeyService::set_update_period(double t)
{
  tick_period = t;
}

void ConfigKeyService::cancel_tick()
{
  if (tick_event)
    mon.timer.cancel_event(tick_event);
  tick_event = nullptr;
}

void ConfigKeyService::tick()
{
  service_tick();
  start_tick();
}

void ConfigKeyService::shutdown()
{
  generic_dout(0) << "quorum service shutdown" << dendl;
  cancel_tick();
  service_shutdown();
}

int ConfigKeyService::store_get(const string &key, bufferlist &bl)
{
  return mon.store->get(CONFIG_PREFIX, key, bl);
}

void ConfigKeyService::get_store_prefixes(set<string>& s) const
{
  s.insert(CONFIG_PREFIX);
}

void ConfigKeyService::store_put(const string &key, bufferlist &bl, Context *cb)
{
  MonitorDBStore::TransactionRef t = paxos.get_pending_transaction();
  t->put(CONFIG_PREFIX, key, bl);
  if (cb)
    paxos.queue_pending_finisher(cb);
  paxos.trigger_propose();
}

void ConfigKeyService::store_delete(const string &key, Context *cb)
{
  MonitorDBStore::TransactionRef t = paxos.get_pending_transaction();
  store_delete(t, key);
  if (cb)
    paxos.queue_pending_finisher(cb);
  paxos.trigger_propose();
}

void ConfigKeyService::store_delete(
    MonitorDBStore::TransactionRef t,
    const string &key)
{
  t->erase(CONFIG_PREFIX, key);
}

bool ConfigKeyService::store_exists(const string &key)
{
  return mon.store->exists(CONFIG_PREFIX, key);
}

void ConfigKeyService::store_list(stringstream &ss)
{
  KeyValueDB::Iterator iter =
    mon.store->get_iterator(CONFIG_PREFIX);

  JSONFormatter f(true);
  f.open_array_section("keys");

  while (iter->valid()) {
    string key(iter->key());
    f.dump_string("key", key);
    iter->next();
  }
  f.close_section();
  f.flush(ss);
}

bool ConfigKeyService::store_has_prefix(const string &prefix)
{
  KeyValueDB::Iterator iter =
    mon.store->get_iterator(CONFIG_PREFIX);

  while (iter->valid()) {
    string key(iter->key());
    size_t p = key.find(prefix);
    if (p != string::npos && p == 0) {
      return true;
    }
    iter->next();
  }
  return false;
}

static bool is_binary_string(const string& s)
{
  for (auto c : s) {
    // \n and \t are escaped in JSON; other control characters are not.
    if ((c < 0x20 && c != '\n' && c != '\t') || c >= 0x7f) {
      return true;
    }
  }
  return false;
}

void ConfigKeyService::store_dump(stringstream &ss, const string& prefix)
{
  KeyValueDB::Iterator iter =
    mon.store->get_iterator(CONFIG_PREFIX);

  dout(10) << __func__ << " prefix '" << prefix << "'" << dendl;
  if (prefix.size()) {
    iter->lower_bound(prefix);
  }

  JSONFormatter f(true);
  f.open_object_section("config-key store");

  while (iter->valid()) {
    if (prefix.size() &&
	iter->key().find(prefix) != 0) {
      break;
    }
    string s = iter->value().to_str();
    if (is_binary_string(s)) {
      ostringstream ss;
      ss << "<<< binary blob of length " << s.size() << " >>>";
      f.dump_string(iter->key().c_str(), ss.str());
    } else {
      f.dump_string(iter->key().c_str(), s);
    }
    iter->next();
  }
  f.close_section();
  f.flush(ss);
}

void ConfigKeyService::store_delete_prefix(
    MonitorDBStore::TransactionRef t,
    const string &prefix)
{
  KeyValueDB::Iterator iter =
    mon.store->get_iterator(CONFIG_PREFIX);

  while (iter->valid()) {
    string key(iter->key());

    size_t p = key.find(prefix);
    if (p != string::npos && p == 0) {
      store_delete(t, key);
    }
    iter->next();
  }
}

bool ConfigKeyService::service_dispatch(MonOpRequestRef op)
{
  Message *m = op->get_req();
  ceph_assert(m != NULL);
  dout(10) << __func__ << " " << *m << dendl;

  if (!in_quorum()) {
    dout(1) << __func__ << " not in quorum -- waiting" << dendl;
    paxos.wait_for_readable(op, new Monitor::C_RetryMessage(&mon, op));
    return false;
  }

  ceph_assert(m->get_type() == MSG_MON_COMMAND);

  MMonCommand *cmd = static_cast<MMonCommand*>(m);

  ceph_assert(!cmd->cmd.empty());

  int ret = 0;
  stringstream ss;
  bufferlist rdata;

  string prefix;
  cmdmap_t cmdmap;

  if (!TOPNSPC::common::cmdmap_from_json(cmd->cmd, &cmdmap, ss)) {
    return false;
  }

  cmd_getval(cmdmap, "prefix", prefix);
  string key;
  cmd_getval(cmdmap, "key", key);

  if (prefix == "config-key get") {
    ret = store_get(key, rdata);
    if (ret < 0) {
      ceph_assert(!rdata.length());
      ss << "error obtaining '" << key << "': " << cpp_strerror(ret);
      goto out;
    }
    ss << "obtained '" << key << "'";

  } else if (prefix == "config-key put" ||
	     prefix == "config-key set") {
    if (!mon.is_leader()) {
      mon.forward_request_leader(op);
      // we forward the message; so return now.
      return true;
    }

    bufferlist data;
    string val;
    if (cmd_getval(cmdmap, "val", val)) {
      // they specified a value in the command instead of a file
      data.append(val);
    } else if (cmd->get_data_len() > 0) {
      // they specified '-i <file>'
      data = cmd->get_data();
    }
    if (data.length() > (size_t) g_conf()->mon_config_key_max_entry_size) {
      ret = -EFBIG; // File too large
      ss << "error: entry size limited to "
         << g_conf()->mon_config_key_max_entry_size << " bytes. "
         << "Use 'mon config key max entry size' to manually adjust";
      goto out;
    }

    ss << "set " << key;

    // we'll reply to the message once the proposal has been handled
    store_put(key, data,
	      new Monitor::C_Command(mon, op, 0, ss.str(), 0));
    // return for now; we'll put the message once it's done.
    return true;

  } else if (prefix == "config-key del" ||
             prefix == "config-key rm") {
    if (!mon.is_leader()) {
      mon.forward_request_leader(op);
      return true;
    }

    if (!store_exists(key)) {
      ret = 0;
      ss << "no such key '" << key << "'";
      goto out;
    }
    store_delete(key, new Monitor::C_Command(mon, op, 0, "key deleted", 0));
    // return for now; we'll put the message once it's done
    return true;

  } else if (prefix == "config-key exists") {
    bool exists = store_exists(key);
    ss << "key '" << key << "'";
    if (exists) {
      ss << " exists";
      ret = 0;
    } else {
      ss << " doesn't exist";
      ret = -ENOENT;
    }

  } else if (prefix == "config-key list" ||
	     prefix == "config-key ls") {
    stringstream tmp_ss;
    store_list(tmp_ss);
    rdata.append(tmp_ss);
    ret = 0;

  } else if (prefix == "config-key dump") {
    string prefix;
    cmd_getval(cmdmap, "key", prefix);
    stringstream tmp_ss;
    store_dump(tmp_ss, prefix);
    rdata.append(tmp_ss);
    ret = 0;

  }

out:
  if (!cmd->get_source().is_mon()) {
    string rs = ss.str();
    mon.reply_command(op, ret, rs, rdata, 0);
  }

  return (ret == 0);
}

string _get_dmcrypt_prefix(const uuid_d& uuid, const string k)
{
  return "dm-crypt/osd/" + stringify(uuid) + "/" + k;
}

int ConfigKeyService::validate_osd_destroy(
    const int32_t id,
    const uuid_d& uuid)
{
  string dmcrypt_prefix = _get_dmcrypt_prefix(uuid, "");
  string daemon_prefix =
    "daemon-private/osd." + stringify(id) + "/";

  if (!store_has_prefix(dmcrypt_prefix) &&
      !store_has_prefix(daemon_prefix)) {
    return -ENOENT;
  }
  return 0;
}

void ConfigKeyService::do_osd_destroy(int32_t id, uuid_d& uuid)
{
  string dmcrypt_prefix = _get_dmcrypt_prefix(uuid, "");
  string daemon_prefix =
    "daemon-private/osd." + stringify(id) + "/";

  MonitorDBStore::TransactionRef t = paxos.get_pending_transaction();
  for (auto p : { dmcrypt_prefix, daemon_prefix }) {
    store_delete_prefix(t, p);
  }

  paxos.trigger_propose();
}

int ConfigKeyService::validate_osd_new(
    const uuid_d& uuid,
    const string& dmcrypt_key,
    stringstream& ss)
{
  string dmcrypt_prefix = _get_dmcrypt_prefix(uuid, "luks");
  bufferlist value;
  value.append(dmcrypt_key);

  if (store_exists(dmcrypt_prefix)) {
    bufferlist existing_value;
    int err = store_get(dmcrypt_prefix, existing_value);
    if (err < 0) {
      dout(10) << __func__ << " unable to get dm-crypt key from store (r = "
               << err << ")" << dendl;
      return err;
    }
    if (existing_value.contents_equal(value)) {
      // both values match; this will be an idempotent op.
      return EEXIST;
    }
    ss << "dm-crypt key already exists and does not match";
    return -EEXIST;
  }
  return 0;
}

void ConfigKeyService::do_osd_new(
    const uuid_d& uuid,
    const string& dmcrypt_key)
{
  ceph_assert(paxos.is_plugged());

  string dmcrypt_key_prefix = _get_dmcrypt_prefix(uuid, "luks");
  bufferlist dmcrypt_key_value;
  dmcrypt_key_value.append(dmcrypt_key);
  // store_put() will call trigger_propose
  store_put(dmcrypt_key_prefix, dmcrypt_key_value, nullptr);
}
