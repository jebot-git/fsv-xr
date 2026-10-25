// SPDX-License-Identifier: MIT
#define GL_GLEXT_PROTOTYPES
#include "gltf-render.h"

#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <tuple>
namespace fsv::gltf {
namespace {
const char *vertexSource = R"GLSL(#version 330 core
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv0;
layout(location=3) in vec2 uv1;
layout(location=4) in vec4 color;
uniform mat4 viewProjection,model;
out vec3 worldPosition,worldNormal;
out vec2 tex0,tex1;
out vec4 vertexColor;
void main(){vec4 p=model*vec4(position,1);worldPosition=p.xyz;worldNormal=transpose(inverse(mat3(model)))*normal;tex0=uv0;tex1=uv1;vertexColor=color;gl_Position=viewProjection*p;gl_PointSize=4;}
)GLSL";
const char *fragmentSource = R"GLSL(#version 330 core
in vec3 worldPosition,worldNormal;
in vec2 tex0,tex1;
in vec4 vertexColor;
out vec4 outputColor;
uniform vec4 baseColor;
uniform vec3 eye,emissive;
uniform float metallic,roughness,alphaCutoff,normalScale,occlusionStrength;
uniform int alphaMode,unlit;
uniform sampler2D colorMap,normalMap,metalMap,emissionMap,occlusionMap;
uniform int useColor,useNormal,useMetal,useEmission,useOcclusion;
uniform int colorUV,normalUV,metalUV,emissionUV,occlusionUV;
uniform mat3 colorTransform,normalTransform,metalTransform,emissionTransform,occlusionTransform;
vec2 uv(int set,mat3 transform){return (transform*vec3(set==0?tex0:tex1,1)).xy;}
vec3 srgb(vec3 c){return pow(max(c,vec3(0)),vec3(2.2));}
vec3 light(vec3 N,vec3 V,vec3 L,vec3 albedo,float metal,float rough){
 vec3 H=normalize(V+L);float nl=max(dot(N,L),0),nv=max(dot(N,V),.001),nh=max(dot(N,H),0),vh=max(dot(V,H),0);
 float a=rough*rough,a2=a*a,d=nh*nh*(a2-1)+1;float D=a2/max(3.14159265*d*d,.0001);
 float k=(rough+1)*(rough+1)/8;float G=(nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k));
 vec3 F0=mix(vec3(.04),albedo,metal);vec3 F=F0+(1-F0)*pow(1-vh,5);
 return ((1-F)*(1-metal)*albedo/3.14159265+D*G*F/max(4*nl*nv,.0001))*nl;
}
void main(){
 vec4 base=baseColor*vertexColor;
 if(useColor!=0){vec4 c=texture(colorMap,uv(colorUV,colorTransform));base*=vec4(srgb(c.rgb),c.a);}
 if(alphaMode==1&&base.a<alphaCutoff)discard;
 float alpha=alphaMode==2?base.a:1;
 vec3 N=normalize(worldNormal);if(!gl_FrontFacing)N=-N;
 if(useNormal!=0){vec2 coord=uv(normalUV,normalTransform);vec3 t=texture(normalMap,coord).xyz*2-1;t.xy*=normalScale;
  vec3 p1=dFdx(worldPosition),p2=dFdy(worldPosition);vec2 d1=dFdx(coord),d2=dFdy(coord);
  vec3 T=p1*d2.y-p2*d1.y;T-=N*dot(N,T);if(dot(T,T)>.0000001){T=normalize(T);vec3 B=normalize(cross(N,T))*sign(d1.x*d2.y-d1.y*d2.x);N=normalize(mat3(T,B,N)*t);}}
 float metal=metallic,rough=roughness;if(useMetal!=0){vec4 c=texture(metalMap,uv(metalUV,metalTransform));metal*=c.b;rough*=c.g;}
 metal=clamp(metal,0,1);rough=clamp(rough,.045,1);
 vec3 emission=emissive;if(useEmission!=0)emission*=srgb(texture(emissionMap,uv(emissionUV,emissionTransform)).rgb);
 vec3 rgb;
 if(unlit!=0)rgb=base.rgb+emission;
 else {vec3 V=normalize(eye-worldPosition);float ao=useOcclusion==0?1:mix(1,texture(occlusionMap,uv(occlusionUV,occlusionTransform)).r,occlusionStrength);
 rgb=base.rgb*(.18*(1-metal)+.035)*ao+emission;
 rgb+=light(N,V,normalize(vec3(-.5,.8,.7)),base.rgb,metal,rough)*3.0;
 rgb+=light(N,V,normalize(vec3(.8,.3,-.6)),base.rgb,metal,rough)*1.4;
 rgb=rgb/(rgb+vec3(1));}
 outputColor=vec4(pow(max(rgb,vec3(0)),vec3(1/2.2))*alpha,alpha);
}
)GLSL";
GLuint shader(GLenum kind, const char *source) {
  GLuint result = glCreateShader(kind);
  glShaderSource(result, 1, &source, nullptr);
  glCompileShader(result);
  GLint status = 0;
  glGetShaderiv(result, GL_COMPILE_STATUS, &status);
  if (!status) {
    char error[2048]{};
    glGetShaderInfoLog(result, sizeof(error), nullptr, error);
    glDeleteShader(result);
    throw std::runtime_error(std::string("glTF shader: ") + error);
  }
  return result;
}
GLuint program() {
  auto v = shader(GL_VERTEX_SHADER, vertexSource),
       f = shader(GL_FRAGMENT_SHADER, fragmentSource);
  auto result = glCreateProgram();
  glAttachShader(result, v);
  glAttachShader(result, f);
  glLinkProgram(result);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint okay = 0;
  glGetProgramiv(result, GL_LINK_STATUS, &okay);
  if (!okay) {
    glDeleteProgram(result);
    throw std::runtime_error("Cannot link glTF preview shaders");
  }
  return result;
}
struct Gpu {
  GLuint vao = 0, vbo = 0, ibo = 0;
};
void destroy(Gpu &g) {
  if (g.vao)
    glDeleteVertexArrays(1, &g.vao);
  if (g.vbo)
    glDeleteBuffers(1, &g.vbo);
  if (g.ibo)
    glDeleteBuffers(1, &g.ibo);
  g = {};
}
void attributes() {
  for (int i = 0; i < 5; ++i)
    glEnableVertexAttribArray(i);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        reinterpret_cast<void *>(offsetof(Vertex, position)));
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        reinterpret_cast<void *>(offsetof(Vertex, normal)));
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        reinterpret_cast<void *>(offsetof(Vertex, uv)));
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        reinterpret_cast<void *>(offsetof(Vertex, uv1)));
  glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        reinterpret_cast<void *>(offsetof(Vertex, color)));
}
Gpu upload(const Primitive &p, const std::vector<Vertex> &vertices,
           bool dynamic) {
  Gpu result;
  glGenVertexArrays(1, &result.vao);
  glBindVertexArray(result.vao);
  glGenBuffers(1, &result.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, result.vbo);
  glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex),
               vertices.data(), dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
  attributes();
  glGenBuffers(1, &result.ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, result.ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, p.indices.size() * sizeof(unsigned),
               p.indices.data(), GL_STATIC_DRAW);
  return result;
}
float determinant(const Mat4 &m) {
  return dot(Vec3{m[0], m[1], m[2]},
             cross({m[4], m[5], m[6]}, {m[8], m[9], m[10]}));
}
} // namespace
struct Renderer::Impl {
  GLuint shader = 0;
  bool owned = true;
  std::shared_ptr<const Model> model;
  Frame frame;
  double seconds = -1;
  int animation = -2;
  std::vector<GLuint> images, samplers;
  std::map<const Primitive *, Gpu> staticMeshes;
  std::map<std::pair<int, int>, Gpu> dynamicMeshes;
  Gpu lines;
  size_t uploaded = 0;
  ~Impl() {
    if (owned)
      clear(true);
  }
  void clear(bool all = false) {
    for (auto &[p, g] : staticMeshes)
      destroy(g);
    for (auto &[key, g] : dynamicMeshes)
      destroy(g);
    staticMeshes.clear();
    dynamicMeshes.clear();
    if (!images.empty())
      glDeleteTextures(images.size(), images.data());
    if (!samplers.empty())
      glDeleteSamplers(samplers.size(), samplers.data());
    images.clear();
    samplers.clear();
    model.reset();
    frame = {};
    seconds = -1;
    animation = -2;
    uploaded = 0;
    if (all) {
      destroy(lines);
      if (shader)
        glDeleteProgram(shader);
      shader = 0;
    }
  }
  GLint uniform(const char *name) { return glGetUniformLocation(shader, name); }
  void integer(const char *name, int value) {
    glUniform1i(uniform(name), value);
  }
  void scalar(const char *name, float value) {
    glUniform1f(uniform(name), value);
  }
  void texture(const TextureSlot &slot, int unit, const char *map,
               const char *use, const char *coord, const char *transform) {
    integer(map, unit);
    integer(use, slot.texture >= 0);
    integer(coord, slot.coord);
    glUniformMatrix3fv(uniform(transform), 1, GL_FALSE, slot.transform.data());
    glActiveTexture(GL_TEXTURE0 + unit);
    if (slot.texture >= 0 && model) {
      auto &t = model->textures.at(slot.texture);
      glBindTexture(GL_TEXTURE_2D, images.at(t.image));
      glBindSampler(unit, samplers.at(slot.texture));
    } else {
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindSampler(unit, 0);
    }
  }
  void material(const Material &m) {
    glUniform4fv(uniform("baseColor"), 1, m.base.data());
    glUniform3f(uniform("emissive"), m.emissive.x, m.emissive.y, m.emissive.z);
    scalar("metallic", m.metallic);
    scalar("roughness", m.roughness);
    scalar("alphaCutoff", m.cutoff);
    integer("alphaMode", m.alpha);
    integer("unlit", m.unlit);
    scalar("normalScale", m.normal.strength);
    scalar("occlusionStrength", m.occlusion.strength);
    texture(m.color, 0, "colorMap", "useColor", "colorUV", "colorTransform");
    texture(m.normal, 1, "normalMap", "useNormal", "normalUV",
            "normalTransform");
    texture(m.metalRough, 2, "metalMap", "useMetal", "metalUV",
            "metalTransform");
    texture(m.emission, 3, "emissionMap", "useEmission", "emissionUV",
            "emissionTransform");
    texture(m.occlusion, 4, "occlusionMap", "useOcclusion", "occlusionUV",
            "occlusionTransform");
  }
};
Renderer::Renderer() : impl(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
void Renderer::clear(bool all) { impl->clear(all); }
void Renderer::abandon() {
  impl->owned = false;
  impl = std::make_unique<Impl>();
}
void Renderer::prepare(std::shared_ptr<const Model> model, double seconds,
                       int animation) {
  auto &s = *impl;
  if (!s.shader)
    s.shader = program();
  if (s.model != model) {
    s.clear();
    s.model = model;
    s.seconds = -1;
    s.animation = -2;
    s.images.resize(model->images.size());
    if (!s.images.empty())
      glGenTextures(s.images.size(), s.images.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (size_t i = 0; i < model->images.size(); ++i) {
      auto &image = model->images[i];
      glBindTexture(GL_TEXTURE_2D, s.images[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
      glGenerateMipmap(GL_TEXTURE_2D);
    }
    s.samplers.resize(model->textures.size());
    if (!s.samplers.empty())
      glGenSamplers(s.samplers.size(), s.samplers.data());
    for (size_t i = 0; i < model->textures.size(); ++i) {
      auto &t = model->textures[i];
      glSamplerParameteri(s.samplers[i], GL_TEXTURE_MAG_FILTER, t.mag);
      glSamplerParameteri(s.samplers[i], GL_TEXTURE_MIN_FILTER, t.min);
      glSamplerParameteri(s.samplers[i], GL_TEXTURE_WRAP_S, t.wrapS);
      glSamplerParameteri(s.samplers[i], GL_TEXTURE_WRAP_T, t.wrapT);
    }
  }
  if (s.animation == animation &&
      (s.seconds == seconds || animation < 0 || model->animations.empty()))
    return;
  s.frame = model->evaluate(seconds, animation);
  s.seconds = seconds;
  s.animation = animation;
  for (auto &draw : s.frame.draws) {
    auto &p = *draw.primitive;
    if (draw.deformed) {
      auto key = std::make_pair(draw.node, draw.primitiveIndex);
      auto it = s.dynamicMeshes.find(key);
      if (it == s.dynamicMeshes.end()) {
        s.dynamicMeshes[key] = upload(p, draw.vertices, true);
        s.uploaded += draw.vertices.size();
      } else {
        glBindBuffer(GL_ARRAY_BUFFER, it->second.vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        draw.vertices.size() * sizeof(Vertex),
                        draw.vertices.data());
      }
    } else if (!s.staticMeshes.count(&p)) {
      s.staticMeshes[&p] = upload(p, p.vertices, false);
      s.uploaded += p.vertices.size();
    }
  }
  glBindVertexArray(0);
  if (glGetError() == GL_OUT_OF_MEMORY)
    throw std::runtime_error("GPU memory exhausted by glTF preview");
}
void Renderer::draw(const Mat4 &vp, const Mat4 &root, Vec3 eye, int width,
                    int height, std::array<float, 4> background) {
  auto &s = *impl;
  glViewport(0, 0, width, height);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glClearColor(background[0], background[1], background[2], background[3]);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (!s.model)
    return;
  glUseProgram(s.shader);
  glUniformMatrix4fv(s.uniform("viewProjection"), 1, GL_FALSE, vp.data());
  glUniform3f(s.uniform("eye"), eye.x, eye.y, eye.z);
  glEnable(GL_PROGRAM_POINT_SIZE);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  Material fallback;
  fallback.metallic = 0;
  std::vector<const Draw *> ordered;
  for (auto &d : s.frame.draws)
    ordered.push_back(&d);
  auto material = [&](const Draw &d) -> const Material & {
    return d.primitive->material < 0
               ? fallback
               : s.model->materials.at(d.primitive->material);
  };
  std::stable_sort(
      ordered.begin(), ordered.end(), [&](const Draw *a, const Draw *b) {
        bool ab = material(*a).alpha == 2, bb = material(*b).alpha == 2;
        if (ab != bb)
          return !ab;
        if (!ab)
          return false;
        auto ac = point(root * a->world, {}), bc = point(root * b->world, {});
        return dot(ac - eye, ac - eye) > dot(bc - eye, bc - eye);
      });
  for (auto draw : ordered) {
    auto &d = *draw;
    auto transform = root * d.world;
    if (std::abs(determinant(transform)) < 1e-12)
      continue;
    auto &m = material(d);
    s.material(m);
    s.integer("unlit", m.unlit || d.primitive->mode != 4);
    glUniformMatrix4fv(s.uniform("model"), 1, GL_FALSE, transform.data());
    if (m.doubleSided)
      glDisable(GL_CULL_FACE);
    else {
      glEnable(GL_CULL_FACE);
      glCullFace(GL_BACK);
    }
    glFrontFace(determinant(transform) < 0 ? GL_CW : GL_CCW);
    if (m.alpha == 2) {
      glEnable(GL_BLEND);
      glDepthMask(GL_FALSE);
    } else {
      glDisable(GL_BLEND);
      glDepthMask(GL_TRUE);
    }
    auto &g = d.deformed ? s.dynamicMeshes.at({d.node, d.primitiveIndex})
                         : s.staticMeshes.at(d.primitive);
    glBindVertexArray(g.vao);
    glDrawElements(d.primitive->mode, d.primitive->indices.size(),
                   GL_UNSIGNED_INT, nullptr);
  }
  glDepthMask(GL_TRUE);
  glFrontFace(GL_CCW);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glBindVertexArray(0);
  glUseProgram(0);
}
void Renderer::controllers(const Mat4 &vp, const std::array<Hand, 2> &hands,
                           Pose object, const Bounds &bounds, int grabbing) {
  auto &s = *impl;
  if (!s.shader)
    return;
  std::vector<Vertex> vertices;
  auto line = [&](Vec3 a, Vec3 b, std::array<float, 4> color) {
    Vertex va, vb;
    va.position = a;
    vb.position = b;
    va.color = vb.color = color;
    vertices.push_back(va);
    vertices.push_back(vb);
  };
  for (int h = 0; h < 2; ++h)
    if (hands[h].valid) {
      auto &hand = hands[h];
      auto tint = (grabbing & (1 << h)) ? std::array<float, 4>{.1f, 1, .25f, 1}
                  : h                   ? std::array<float, 4>{1, .48f, .1f, 1}
                                        : std::array<float, 4>{.2f, .55f, 1, 1};
      for (int i = 0; i < 24; ++i) {
        float a = i * 6.2831853f / 24, b = (i + 1) * 6.2831853f / 24;
        line(transform(hand.grip, {.045f * std::cos(a),
                                   .045f * std::sin(a) + .035f, -.04f}),
             transform(hand.grip, {.045f * std::cos(b),
                                   .045f * std::sin(b) + .035f, -.04f}),
             tint);
      }
      line(transform(hand.grip, {0, -.05f, .02f}),
           transform(hand.grip, {0, .025f, -.03f}), tint);
      if (hand.aimValid)
        line(hand.aim.position, transform(hand.aim, {0, 0, -2}), tint);
    }
  if (grabbing && bounds.valid) {
    Vec3 corners[8];
    for (int i = 0; i < 8; ++i)
      corners[i] = transform(object, {i & 1 ? bounds.max.x : bounds.min.x,
                                      i & 2 ? bounds.max.y : bounds.min.y,
                                      i & 4 ? bounds.max.z : bounds.min.z});
    for (int i = 0; i < 8; ++i)
      for (int bit : {1, 2, 4})
        if (!(i & bit))
          line(corners[i], corners[i | bit], {.1f, 1, .25f, 1});
  }
  if (vertices.empty())
    return;
  if (!s.lines.vao) {
    Primitive p;
    p.indices.resize(vertices.size());
    for (size_t i = 0; i < p.indices.size(); ++i)
      p.indices[i] = i;
    s.lines = upload(p, vertices, true);
  }
  glBindVertexArray(s.lines.vao);
  glBindBuffer(GL_ARRAY_BUFFER, s.lines.vbo);
  glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex),
               vertices.data(), GL_STREAM_DRAW);
  glUseProgram(s.shader);
  Material m;
  m.unlit = true;
  m.metallic = 0;
  s.material(m);
  Mat4 identity;
  glUniformMatrix4fv(s.uniform("model"), 1, GL_FALSE, identity.data());
  glUniformMatrix4fv(s.uniform("viewProjection"), 1, GL_FALSE, vp.data());
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glLineWidth(2);
  glDrawArrays(GL_LINES, 0, vertices.size());
  glBindVertexArray(0);
  glUseProgram(0);
}
size_t Renderer::uploadedVertices() const { return impl->uploaded; }
} // namespace fsv::gltf
