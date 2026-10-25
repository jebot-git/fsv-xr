// SPDX-License-Identifier: LGPL-2.1-or-later
#include "thumbnail-decode.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/resource.h>
int main(int argc,char **argv){
  if(argc!=2)return 2;
  fsv_thumbnail result{};
  bool ok=fsv_thumbnail_decode(argv[1],256,96,&result,nullptr,nullptr);
  rusage usage{};getrusage(RUSAGE_SELF,&usage);
  // ru_maxrss can include the pre-exec parent address space after posix_spawn.
  // VmHWM measures this executable's actual decode address space on Linux.
  std::ifstream status("/proc/self/status");std::string line;
  while(std::getline(status,line))if(line.rfind("VmHWM:",0)==0){std::sscanf(line.c_str(),"VmHWM: %ld",&usage.ru_maxrss);break;}
  printf("{\"ok\":%s,\"source_width\":%d,\"source_height\":%d,\"width\":%d,\"height\":%d,\"decoder_width\":%d,\"peak_rss_kib\":%ld,\"first\":[%u,%u,%u,%u]}\n",ok?"true":"false",result.source_width,result.source_height,result.width,result.height,result.decoder_width,usage.ru_maxrss,result.rgba?result.rgba[0]:0,result.rgba?result.rgba[1]:0,result.rgba?result.rgba[2]:0,result.rgba?result.rgba[3]:0);
  fsv_thumbnail_free(&result);
}
