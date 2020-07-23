//
// Created by Zhichao Cao czc199182@gmail.com 07/18/2020.
//

#include "zone_mapping.h"

namespace leveldb {

ZoneMapping::ZoneMapping(std::shared_ptr<ZoneNamespace> zns, int zone_num) {
  zns_ptr_ = zns;
  zone_num_ = zone_num;

  // Initilize the zone info list
  for(int i = 0; i < zone_num_; i++) {
    ZnsZoneInfo z_info;
    std::unordered_map<std::string, ZnsFileInfo*> tmp_map;
    z_info.zone_id = i;
    //z_info.zone_ptr = zns_ptr_->GetZone(i);
    z_info.valid_size = 0;
    z_info.valid_size = 0;
    z_info.files_map = tmp_map;
    zone_list_.push_back(z_info);
  }

  // store the zone pointers in the empty_zones_
  for(int i = 0; i < zone_num_; i++) {
    empty_zones_.insert(std::make_pair(i, &zone_list_[i]));
  }
}

Status ZoneMapping::GetAndUseOneEmptyZone(ZnsZoneInfo** z_info_ptr) {
  if (empty_zones_.size() == 0) {
    return Status::NotFound("invalid");
  }

  auto tmp = empty_zones_.begin();
  z_info_ptr = &(tmp->second);
  empty_zones_.erase(tmp->second->zone_id);
  used_zones_.insert(std::make_pair(tmp->second->zone_id, tmp->second));
  return Status::OK();
}

Status ZoneMapping::CreateFileOnZone(Env* env, std::string file_name, int zone_id, int& offset) {
  auto found = files_map_.find(file_name);
  auto z = used_zones_.find(zone_id);
  if (found != files_map_.end() || z == used_zones_.end()) {
    return Status::InvalidArgument("invalid");
  }

  // Increase the valid file number
  z->second->valid_file_num += 1;
  auto zone_ptr = z->second->zone_ptr;
  ZoneInfo z_info = zone_ptr->ReportZone();
  ZnsFileInfo tmp_info;
  tmp_info.file_name = file_name;
  tmp_info.zone_id = z_info.id;
  tmp_info.offset = z_info.write_pointer;
  tmp_info.length = 0;
  tmp_info.f_stat = ZnsFileStat::kCreated;
  tmp_info.create_time = env->NowMicros();
  tmp_info.delete_time = 0;
  files_map_.insert(std::make_pair(file_name, tmp_info));
  z->second->files_map.insert(std::make_pair(file_name, &(files_map_[file_name])));
  return Status::OK();
}

Status ZoneMapping::DeleteFileOnZone(Env* env, std::string file_name) {
  auto found = files_map_.find(file_name);
  if (found == files_map_.end()) {
    return Status::OK();
  }
  auto z = used_zones_.find(found->second.zone_id);
  if (z == used_zones_.end()) {
    return Status::Corruption("invalid");
  }

  // update the zone info;
  z->second->valid_size -= found->second.length;
  z->second->valid_file_num -=1;
  z->second->files_map.erase(file_name);
  auto z_info = z->second;
  if (z_info->valid_size == 0) {
    z_info->zone_ptr->ResetWritePointer();
    empty_zones_.insert(std::make_pair(z_info->zone_id, z_info));
    used_zones_.erase(z_info->zone_id);
  }

  // Update the file info
  found->second.f_stat = ZnsFileStat::kDeleted;
  found->second.delete_time = env->NowMicros();
  found->second.length = 0;
  found->second.offset = 0;
  found->second.zone_id = -1;
  return Status::OK();
}

Status ZoneMapping::CloseFileOnZone(std::string file_name) {
  auto found = files_map_.find(file_name);
  if (found == files_map_.end()) {
    return Status::InvalidArgument("invalid");
  }
  if (found->second.f_stat == ZnsFileStat::kDeleted) {
    return Status::InvalidArgument("invalid");
  }
  auto z = used_zones_.find(found->second.zone_id);
  if (z == used_zones_.end()) {
    return Status::Corruption("invalid");
  }
  found->second.f_stat = ZnsFileStat::kClosed;

  // Only when the file is closed, we think the write is done
  // So the data is valid for the zone, otherwise, it is invalid
  // can be cleaned.
  z->second->valid_size += found->second.length;
  return Status::OK();
}

Status ZoneMapping::ReadFileOnZone(std::string file_name, size_t offset,
                                    size_t len, const char *buffer) {
  auto found = files_map_.find(file_name);
  if (found == files_map_.end()) {
    return Status::InvalidArgument("invalid");
  }
  if (found->second.f_stat == ZnsFileStat::kDeleted) {
    return Status::InvalidArgument("invalid");
  }
  auto z = used_zones_.find(found->second.zone_id);
  if (z == used_zones_.end()) {
    return Status::Corruption("invalid");
  }

 if (offset > found->second.length) {
    return Status::InvalidArgument("invalid");
  }

  size_t valid_len;
  if (offset+len > found->second.length) {
    valid_len = found->second.length - offset;
  } else {
    valid_len = len;
  }

  ZoneAddress z_address;
  z_address.zone_id = z->second->zone_id;
  z_address.offset = found->second.offset + offset;
  z_address.length = valid_len;
  char *data = (char*)buffer;
  return z->second->zone_ptr->ZoneRead(z_address, data);
}

Status ZoneMapping::WriteFileOnZone(std::string file_name,
                                  size_t len, const char *buffer) {
  auto found = files_map_.find(file_name);
  if (found == files_map_.end()) {
    return Status::InvalidArgument("invalid");
  }
  if (found->second.f_stat == ZnsFileStat::kDeleted) {
    return Status::InvalidArgument("invalid");
  }
  auto z = used_zones_.find(found->second.zone_id);
  if (z == used_zones_.end()) {
    return Status::Corruption("invalid");
  }

  ZoneInfo z_info = z->second->zone_ptr->ReportZone();

  ZoneAddress z_address;
  z_address.zone_id = z->second->zone_id;
  z_address.offset = z_info.write_pointer;
  z_address.length = len;
  Status s = z->second->zone_ptr->ZoneWrite(z_address, buffer);
  if (!s.ok()) {
    return s;
  }

  // Updates the metadata
  found->second.length += len;
  z->second->valid_size += len;
  return Status::OK();
}

bool ZoneMapping::IsFileInZone(std::string file_name) {
  auto found = files_map_.find(file_name);
  if (found == files_map_.end()) {
    return false;
  }

  if(found->second.f_stat == ZnsFileStat::kDeleted) {
    return false;
  }
  return true;
}

} //name space