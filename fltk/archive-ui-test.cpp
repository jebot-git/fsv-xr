// SPDX-License-Identifier: LGPL-2.1-or-later
#define main fsvr_application_main
#include "main.cpp"
#undef main
#include <iostream>
#include <unistd.h>
static void require(bool b,const char *m){if(!b)throw std::runtime_error(m);}
static void wait(Browser &browser){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(25);while(!browser.idle()){require(std::chrono::steady_clock::now()<end,"Archive UI timed out");Fl::wait(.01);}Fl::check();}
static void each(Fl_Group &group,const std::function<void(Fl_Widget *)> &fn){for(int i=0;i<group.children();++i){auto w=group.child(i);fn(w);if(auto nested=dynamic_cast<Fl_Group *>(w))each(*nested,fn);}}
static void answer(void *){
  for(auto window=Fl::first_window();window;window=Fl::next_window(window))if(window->shown()){
    Fl_Input_ *input=nullptr;Fl_Button *ok=nullptr;
    each(*window,[&](Fl_Widget *w){if(auto p=dynamic_cast<Fl_Input_ *>(w);p && p->type()==FL_SECRET_INPUT)input=p;if(auto p=dynamic_cast<Fl_Button *>(w);p && p->label() && std::string(p->label())=="OK")ok=p;});
    if(input && ok){input->value("fixture-password");ok->do_callback();return;}
  }
  Fl::repeat_timeout(.03,answer);
}
static fs::path chosenDestination;
static void chooseExtraction(void *) {
  for(auto window=Fl::first_window();window;window=Fl::next_window(window))
    if(window->shown() && window->label() && std::string(window->label())=="Extract archive") {
      Fl_Input *input=nullptr;Fl_Button *extract=nullptr;
      each(*window,[&](Fl_Widget *w){if(auto p=dynamic_cast<Fl_Input *>(w))input=p;if(auto p=dynamic_cast<Fl_Button *>(w);p && p->label() && std::string(p->label())=="Extract")extract=p;});
      if(input && extract){input->value(chosenDestination.c_str());extract->do_callback();return;}
    }
  Fl::repeat_timeout(.03,chooseExtraction);
}
static bool choicesChecked=false;
static void cancelCreate(void *){
  for(auto window=Fl::first_window();window;window=Fl::next_window(window))if(window->shown() && window->label() && std::string(window->label())=="Create archive"){
    Fl_Choice *choice=nullptr;Fl_Button *cancel=nullptr;
    each(*window,[&](Fl_Widget *w){if(auto p=dynamic_cast<Fl_Choice *>(w))choice=p;if(auto p=dynamic_cast<Fl_Button *>(w);p && p->label() && std::string(p->label())=="Cancel")cancel=p;});
    if(choice && cancel){choicesChecked=choice->size()>20;cancel->do_callback();return;}
  }
  Fl::repeat_timeout(.03,cancelCreate);
}
int main(int argc,char **argv)try{
  char temporary[]="/tmp/fsvr-archive-ui-XXXXXX";require(mkdtemp(temporary),"temporary");fs::path root=temporary;
  setenv("XDG_CONFIG_HOME",(root/"config").c_str(),1);setenv("XDG_STATE_HOME",(root/"state").c_str(),1);
  fs::create_directory(root/"source");std::ofstream(root/"source/fixture ü.txt")<<"payload";
  fsv::ArchiveOptions options;options.password="fixture-password";
  auto archive=root/"name with spaces $ ü.zip";fsv::compress(std::vector<fs::path>{root/"source"},archive,options);
  dx::init();fl_register_images();
  {Browser browser;browser.show();gchar *uri=g_filename_to_uri(archive.c_str(),nullptr,nullptr);require(uri,"file URL");browser.openArguments({uri});g_free(uri);wait(browser);require(!browser.currentScene().entries.empty(),"archive URL browsing");
    Fl::add_timeout(.05,answer);browser.command("Extract archive");wait(browser);Fl::remove_timeout(answer);
    require(fs::file_size(fsv::extractionDestination(archive)/"source/fixture ü.txt")==7,"native password retry and extraction");
    chosenDestination=root/"chosen extraction ü";Fl::add_timeout(.05,chooseExtraction);Fl::add_timeout(.1,answer);browser.command("Extract to...");wait(browser);Fl::remove_timeout(chooseExtraction);Fl::remove_timeout(answer);
    require(fs::file_size(chosenDestination/"source/fixture ü.txt")==7,"chosen extraction directory");
    browser.load({root});wait(browser);SpatialView *view=nullptr;each(browser,[&](Fl_Widget *w){if(auto p=dynamic_cast<SpatialView *>(w))view=p;});require(view,"spatial view");int index=-1;for(size_t i=0;i<browser.currentScene().entries.size();++i)if(browser.currentScene().entries[i].name=="source" && browser.currentScene().entries[i].parent==-1)index=int(i);require(index>=0,"source selection");view->selectionChanged(index);
    Fl::add_timeout(.05,cancelCreate);browser.command("Compress...");Fl::remove_timeout(cancelCreate);require(choicesChecked,"format chooser exposes broad writers");
    if(argc>1)browser.smoke(argv[1]);browser.hide();
  }
  fs::remove_all(root);std::cout<<"PASS native archive file URL, encrypted extraction/password dialog, format chooser and cancellation\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
