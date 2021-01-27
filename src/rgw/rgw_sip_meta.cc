
#include "common/debug.h"
#include "common/ceph_json.h"
#include "common/Formatter.h"

#include "rgw_sip_meta.h"
#include "rgw_metadata.h"
#include "rgw_mdlog.h"
#include "rgw_b64.h"

#include "services/svc_mdlog.h"

#define dout_subsys ceph_subsys_rgw

void siprovider_meta_info::dump(Formatter *f) const
{
  encode_json("section", section, f);
  encode_json("id", id, f);
}

void siprovider_meta_info::decode_json(JSONObj *obj)
{
  JSONDecoder::decode_json("section", section, obj);
  JSONDecoder::decode_json("id", id, obj);
}

int SIProvider_MetaFull::init()
{
  int r = get_all_sections();
  if (r < 0) {
    lderr(cct) << __func__ << "(): get_all_sections() returned r=" << r << dendl;
    return r;
  }

  rearrange_sections();

  std::string prev;

  for (auto& s : sections) {
    next_section_map[prev] = s;
    prev = s;
  }

  return 0;
}

void SIProvider_MetaFull::append_section_from_set(set<string>& all_sections, const string& name) {
  set<string>::iterator iter = all_sections.find(name);
  if (iter != all_sections.end()) {
    sections.emplace_back(std::move(*iter));
    all_sections.erase(iter);
  }
}

/*
 * meta sync should go in the following order: user, bucket.instance, bucket
 * then whatever other sections exist (if any)
 */
void SIProvider_MetaFull::rearrange_sections() {
  set<string> all_sections;
  std::move(sections.begin(), sections.end(),
            std::inserter(all_sections, all_sections.end()));
  sections.clear();

  append_section_from_set(all_sections, "user");
  append_section_from_set(all_sections, "bucket.instance");
  append_section_from_set(all_sections, "bucket");

  std::move(all_sections.begin(), all_sections.end(),
            std::back_inserter(sections));
}

int SIProvider_MetaFull::get_all_sections() {
  void *handle;

  int ret = meta.mgr->list_keys_init(string(), string(), &handle); /* iterate top handler */
  if (ret < 0) {
    lderr(cct) << "ERROR: " << __func__ << "(): list_keys_init() returned ret=" << ret << dendl;
    return ret;
  }

  std::list<string> result;
  bool truncated;
  int max = 32;

  do {
    ret = meta.mgr->list_keys_next(handle, max, result,
                                   &truncated);
    if (ret < 0) {
      lderr(cct) << "ERROR: " << __func__ << "(): list_keys_init() returned ret=" << ret << dendl;
      return ret;
    }
    std::move(result.begin(), result.end(),
              std::inserter(sections, sections.end()));
    result.clear();
  } while (truncated);

  meta.mgr->list_keys_complete(handle);

  return 0;
}

int SIProvider_MetaFull::next_section(const std::string& section, string *next)
{
  auto iter = next_section_map.find(section);
  if (iter == next_section_map.end()) {
    if (section.empty()) {
      ldout(cct, 5) << "ERROR: " << __func__ << "(): next_section_map() is not initialized" << dendl;
      return -EINVAL;
    }
    return -ENOENT;
  }
  *next = iter->second;
  return 0;
}

std::string SIProvider_MetaFull::to_marker(const std::string& section, const std::string& k) const
{
  return section + "/" + k;
}

int SIProvider_MetaFull::do_fetch(int shard_id, std::string marker, int max, fetch_result *result)
{
  if (shard_id > 0) {
    return -ERANGE;
  }

  string section;
  string m;

  if (!marker.empty()) {
    auto pos = marker.find("/");
    if (pos == string::npos) {
      return -EINVAL;
    }
    section = marker.substr(0, pos);
    m = marker.substr(pos + 1);
  } else {
    int r = next_section(section, &section);
    if (r < 0) {
      return r;
    }
  }

  void *handle = nullptr;

  result->done = false;
  result->more = true;

  bool new_section = true;

  while (max > 0) {
    int ret;
    if (new_section) {
      ret = meta.mgr->list_keys_init(section, m, &handle);
      if (ret < 0) {
        lderr(cct) << "ERROR: " << __func__ << "(): list_keys_init() returned ret=" << ret << dendl;
        return ret;
      }
      new_section = false;
    }

    std::list<RGWMetadataHandler::KeyInfo> entries;
    bool truncated;

    ret = meta.mgr->list_keys_next(handle, max, entries,
                                   &truncated);
    if (ret < 0) {
      lderr(cct) << "ERROR: " << __func__ << "(): list_keys_init() returned ret=" << ret << dendl;
      return ret;
    }

    if (!entries.empty()) {
      max -= entries.size();

      m = entries.back().marker;

      for (auto& k : entries) {
        auto e = create_entry(section, k.key, k.marker);
        result->entries.push_back(e);
      }
    }

    if (!truncated) {
      ret = next_section(section, &section);
      if (ret == -ENOENT) {
        result->done = true;
        result->more = false;
        break;
      }
      meta.mgr->list_keys_complete(handle);
      handle = nullptr;
      m.clear();
      new_section = true;
    }
  }

  if (handle) {
    meta.mgr->list_keys_complete(handle);
  }

  return 0;
}

SIProvider_MetaInc::SIProvider_MetaInc(CephContext *_cct,
				       RGWSI_MDLog *_mdlog,
				       const string& _period_id) : SIProvider_SingleStage(_cct,
                                                                                          "meta.inc",
                                                                                          std::nullopt,
                                                                                          std::make_shared<SITypeHandlerProvider_Default<siprovider_meta_info> >(),
                                                                                          std::nullopt, /* stage id */
                                                                                          SIProvider::StageType::INC,
                                                                                          _cct->_conf->rgw_md_log_max_shards,
                                                                                          false),
                                                                   mdlog(_mdlog),
                                                                   period_id(_period_id) {}

int SIProvider_MetaInc::init()
{
  meta_log = mdlog->get_log(period_id);
  return 0;
}

int SIProvider_MetaInc::do_fetch(int shard_id, std::string marker, int max, fetch_result *result)
{
  if (shard_id >= stage_info.num_shards) {
    return -ERANGE;
  }

  utime_t start_time;
  utime_t end_time;

  void *handle;

  meta_log->init_list_entries(shard_id, start_time.to_real_time(), end_time.to_real_time(), marker, &handle);
  bool truncated;
  do {
    list<cls_log_entry> entries;
    int ret = meta_log->list_entries(handle, max, entries, NULL, &truncated);
    if (ret < 0) {
      lderr(cct) << "ERROR: meta_log->list_entries() failed: ret=" << ret << dendl;
      return -ret;
    }

    max -= entries.size();

    for (auto& entry : entries) {
      siprovider_meta_info meta_info(entry.section, entry.name);

      SIProvider::Entry e;
      e.key = entry.id;
      meta_info.encode(e.data);
      result->entries.push_back(e);
    }
  } while (truncated && max > 0);

  result->done = false; /* FIXME */
  result->more = truncated;

  meta_log->complete_list_entries(handle);

  return 0;
}


int SIProvider_MetaInc::do_get_start_marker(int shard_id, std::string *marker, ceph::real_time *timestamp) const
{
  marker->clear();
  *timestamp = ceph::real_time();
  return 0;
}

int SIProvider_MetaInc::do_get_cur_state(int shard_id, std::string *marker, ceph::real_time *timestamp,
                                         bool *disabled, optional_yield y) const
{
  RGWMetadataLogInfo info;

  int ret = meta_log->get_info(shard_id, &info);
  if (ret < 0) {
    ldout(cct, 0) << "ERROR: failed to get meta log info for shard_id=" << shard_id << ": ret=" << ret << dendl;
    return ret;
  }

  *marker = info.marker;
  *timestamp = info.last_update;
  *disabled = false;

  return 0;
}

int SIProvider_MetaInc::do_trim(int shard_id, const std::string& marker)
{
  utime_t start_time, end_time;
  int ret;
  // trim until -ENODATA
  do {
    ret = meta_log->trim(shard_id, start_time.to_real_time(),
                         end_time.to_real_time(), string(), marker);
  } while (ret == 0);
  if (ret < 0 && ret != -ENODATA) {
    ldout(cct, 20) << "ERROR: meta_log->trim(): returned ret=" << ret << dendl;
    return ret;
  }
  return 0;
}
