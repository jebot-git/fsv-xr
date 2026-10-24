// SPDX-License-Identifier: MIT
#define main fsvr_application_main
#include "main.cpp"
#undef main
#include <iostream>
#include <set>
static void require(bool v, const char *message) { if (!v) throw std::runtime_error(message); }
template<class F> void waitFor(F f) {
  auto end = std::chrono::steady_clock::now()+std::chrono::seconds(15);
  while (!f()) { require(std::chrono::steady_clock::now()<end,"Stereo UI timeout"); Fl::wait(.02); }
}
int main(int argc,char **argv) try {
  require(argc==3,"Expected fixture and output directory");
  dx::init();
  Browser browser; browser.show(); browser.load({argv[1]});
  fs::path output=argv[2];
  waitFor([&] { return browser.idle() && browser.stereoStatus()["frames"].get<int>()>5; });
  auto state=browser.stereoStatus();
  require(state["tracked"] && state["curved"] && state["picking"],"Missing tracked cylinder stereo");
  require(state["error"]=="","Stereo rendering error");
  browser.make_current(); browser.draw();
  int w=browser.pixel_w(),h=browser.pixel_h(),half=w/2;
  std::vector<unsigned char> pixels(size_t(w)*h*3);
  glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
  size_t differing=0;
  // Only the scene has binocular disparity; the top row of controls is identical.
  for(int y=0;y<h;++y) for(int x=0;x<half;++x) {
    auto a=&pixels[(size_t(y)*w+x)*3], b=&pixels[(size_t(y)*w+w-half+x)*3];
    bool different=std::abs(int(a[0])-b[0])+std::abs(int(a[1])-b[1])+std::abs(int(a[2])-b[2])>24;
    if(y>h-60) require(!different,"Stereo chrome differs between eyes");
    differing+=different;
  }
  require(differing>300,"Eye images have no scene disparity");
  dx::screenshot(browser,(output/"stereo.ppm").c_str());
  SpatialView *view=nullptr;
  std::function<void(Fl_Group &)> find=[&](Fl_Group &group) {
    for(int i=0;i<group.children();++i) {
      if(auto v=dynamic_cast<SpatialView *>(group.child(i))) view=v;
      else if(auto nested=dynamic_cast<Fl_Group *>(group.child(i))) find(*nested);
    }
  };
  find(browser); require(view,"Missing spatial view");
  std::set<int> hits;
  for(int y=0;y<view->h();y+=8) for(int x=0;x<view->w();x+=8) {
    int hit=view->pickAt(x,y); if(hit>=0) hits.insert(hit);
  }
  require(hits.size()>=4,"Stereo pointer rays miss the scene");
  browser.command("Stereo 3D");
  browser.make_current();browser.draw();
  require(!browser.stereoStatus()["enabled"] && !browser.stereoStatus()["picking"],"Depth toggle failed");
  std::ofstream(output/"toggle-ready") << browser.stereoStatus();
  waitFor([&] { return !browser.stereoStatus()["xr"].get<bool>(); });
  require(!browser.stereoStatus()["picking"],"Desktop retained stereo picking");
  browser.hide();
  Browser restored;
  require(!restored.stereoStatus()["enabled"],"Stereo preference was not saved");
  std::cout << "PASS tracked in-window stereo, cylinder warp, identical chrome, depth toggle, desktop return and saved preference\n";
} catch(const std::exception &e) { std::cerr << e.what()<<'\n';return 1; }
