#ifndef MEMTOOL_MEM_EXTENDS
#define MEMTOOL_MEM_EXTENDS

#include <unordered_map>

#include "memextend.hpp"

#define PERMS_PROT_ASSIGN(perms, index, perm, prot, val)                       \
  if (perms[index] == perm)                                                    \
  val |= prot

int memtool::extend::get_perms_prot(char *perms) {
  int prot;

  prot = PROT_NONE;
  PERMS_PROT_ASSIGN(perms, 0, 'r', PROT_READ, prot);
  PERMS_PROT_ASSIGN(perms, 1, 'w', PROT_WRITE, prot);
  PERMS_PROT_ASSIGN(perms, 2, 'x', PROT_EXEC, prot);
  return prot;
}

int memtool::extend::det_mem_range(char *name, char *prems) {
  if (strlen(name) == 0)
    return memsetting::Anonymous;

  if (strcmp(name, "[heap]") == 0)
    return memsetting::C_heap;

  if (strncmp(name, "[anon:libc_malloc", 17) == 0 ||
      strncmp(name, "[anon:scudo:", 13) == 0)
    return memsetting::C_alloc;

  if ((strstr(name, "/data/app/") != nullptr) &&
      strstr(prems, "xp") != nullptr && strstr(name, ".so") != nullptr)
    return memsetting::Code_app;

  if (strstr(name, "/system/framework/") != nullptr)
    return memsetting::Code_system;

  if (strstr(name, "[anon:.bss]") != nullptr)
    return memsetting::C_bss;

  if (strstr(name, "/data/app/") != nullptr && strstr(name, ".so") != nullptr)
    return memsetting::C_data;

  return memsetting::Other;
}

void memtool::extend::set_mem_ranges(int ranges) {
  vm_area_vec.clear();

  for (auto vma : vm_area_list) {
    if (ranges & vma->range) {
      vm_area_vec.emplace_back(vma);
    }
  }
}

int memtool::extend::parse_process_module() {
  if (vm_area_list.empty())
    return -1;

  // 静态区域 默认cd xa 还有对应的cb-> libxx.so.bss
  auto static_cd_xa = memsetting::C_data | memsetting::Code_app;

  std::unordered_map<std::string, int> module_map;

  utils::free_container_data(vm_static_list);
  vm_static_list.clear();

  auto prev = vm_area_list.front();
  for (auto curr : vm_area_list) {
    if (curr->range & static_cd_xa) {
      std::string name = std::string(curr->name);
      auto pos = name.find_last_of("/");
      if (pos != std::string::npos) {
        name = name.substr(pos + 1);
      }
      module_map.find(name) == module_map.end() ? module_map[name] = 1
                                                : ++module_map[name];
      auto mod = new vm_static_data(curr->start, curr->end, curr->range);
      strcpy(mod->name, name.c_str());
      mod->count = module_map[name];
      // printf("mod->name %s count %d\n", mod->name, mod->count);
      vm_static_list.emplace_back(mod);
    }

    else if (curr->range & memsetting::C_bss && prev->range & static_cd_xa) {
      std::string name = std::string(prev->name);
      auto pos = name.find_last_of("/");
      if (pos != std::string::npos) {
        name = name.substr(pos + 1);
      }
      name += ":bss";
      module_map.find(name) == module_map.end() ? module_map[name] = 1
                                                : ++module_map[name];
      auto mod = new vm_static_data(curr->start, curr->end, curr->range);
      strcpy(mod->name, name.c_str());
      mod->count = module_map[name];
      // printf("mod->name %s count %d\n", mod->name, mod->count);
      vm_static_list.emplace_back(mod);
    }

    prev = curr;
  }
  return 0;
}

int memtool::extend::parse_process_maps() {

  char path[64];

  utils::free_container_data(vm_area_list);
  vm_area_list.clear();

  snprintf(path, sizeof(path), "/proc/%d/maps", target_pid);
  auto add_vma_list = [](auto &vma) {
    vm_area_data *v = new vm_area_data();
    memcpy(v, &vma, sizeof(vma));
    vm_area_list.emplace_back(v);
  };

  FILE *f;
  vm_area_data vma;
  char *line;
  size_t len;

  f = fopen(path, "r");
  if (f == nullptr)
    return -1;

  line = nullptr;
  len = 0;
  while (getline(&line, &len, f) > 0) {
    *vma.name = 0;
    sscanf(line, "%lx-%lx %s %lx %s %lu %127s\n", &vma.start, &vma.end,
           vma.perms, &vma.offset, vma.dev, &vma.inode, vma.name);
    // if (strstr(vma.perms, "r") == nullptr)
    //    continue;
    // printf("vma.name %s\n", vma.name); //vma.name [page 存在解析不全问题
    vma.range = det_mem_range(vma.name, vma.perms);
    vma.prot = get_perms_prot(vma.perms);

    add_vma_list(vma);
  }

  free(line);
  return 0;
}

int memtool::extend::get_target_mem() {
  return parse_process_maps() || parse_process_module();
}

memtool::extend::extend() {}

memtool::extend::~extend() {
  // 释放内存
  utils::free_container_data(vm_area_list);
  vm_area_list.clear();
  utils::free_container_data(vm_static_list);
  vm_static_list.clear();
}

#endif
