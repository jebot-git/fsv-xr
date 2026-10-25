// SPDX-License-Identifier: LGPL-2.1-or-later
#define main fsvr_application_main
#include "main.cpp"
#undef main
#include <png.h>
#include <iostream>
#include <unistd.h>
static void require(bool b,const char *message){if(!b)throw std::runtime_error(message);}
static void wait(Browser &browser){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(!browser.idle()){require(std::chrono::steady_clock::now()<deadline,"Folder scan timed out");Fl::wait(.005);}Fl::check();}
static void makePicture(const fs::path &path){
  auto file=fopen(path.c_str(),"wb");require(file,"PNG fixture");auto png=png_create_write_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);auto info=png_create_info_struct(png);require(png && info,"PNG writer");if(setjmp(png_jmpbuf(png)))throw std::runtime_error("PNG fixture failed");png_init_io(png,file);png_set_IHDR(png,info,10000,10000,8,PNG_COLOR_TYPE_RGB,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);png_write_info(png,info);std::vector<unsigned char> row(30000,140);for(int y=0;y<10000;++y)png_write_row(png,row.data());png_write_end(png,info);png_destroy_write_struct(&png,&info);fclose(file);
}
int main(int argc,char **argv)try{
  char tmp[]="/tmp/fsvr-preview-navigation-XXXXXX";require(mkdtemp(tmp),"temporary");fs::path root=tmp;
  setenv("XDG_CONFIG_HOME",(root/"config").c_str(),1);setenv("XDG_STATE_HOME",(root/"state").c_str(),1);
  fs::create_directories(root/"pictures");fs::create_directories(root/"empty");makePicture(root/"large.png");
  for(int i=0;i<40;++i)fs::create_hard_link(root/"large.png",root/"pictures"/("picture-"+std::to_string(i)+".png"));
  dx::init();fl_register_images();
  {Browser browser;browser.show();
    for(int i=0;i<24;++i){
      browser.setLayout(i%3==0?"map":i%3==1?"tree":"disc");browser.load({root/"pictures"});wait(browser);
      for(int n=0;n<6;++n)Fl::wait(.01);
      browser.command("Up");wait(browser);browser.command("Back");wait(browser);browser.command("Forward");wait(browser);
      browser.load({root/"empty"});wait(browser);
    }
    if(argc>1){auto pictures=fsv::locationArgument(argv[1]);for(int i=0;i<8;++i){browser.setLayout(i%3==0?"map":i%3==1?"tree":"disc");browser.load(pictures);wait(browser);for(int n=0;n<8;++n)Fl::wait(.01);browser.load({root/"empty"});wait(browser);}}
    browser.hide();
  }
  fs::remove_all(root);
  std::cout<<"PASS repeated image-heavy folder changes, layouts, history and thumbnail cancellation\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
