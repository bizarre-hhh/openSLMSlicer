#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include "pyslm/support_render.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace pyslm::support {
namespace {
std::mutex renderMutex;
template<class T>T loadGL(const char* name) {
    auto pointer=wglGetProcAddress(name);
    if(!pointer||pointer==reinterpret_cast<PROC>(1)||pointer==reinterpret_cast<PROC>(2)||
       pointer==reinterpret_cast<PROC>(3)||pointer==reinterpret_cast<PROC>(-1))
        throw std::runtime_error(std::string("OpenGL entry point unavailable: ")+name);
    return reinterpret_cast<T>(pointer);
}
struct Context {
    HWND window=nullptr;HDC dc=nullptr;HGLRC rc=nullptr;
    HDC previousDC=wglGetCurrentDC();HGLRC previousRC=wglGetCurrentContext();
    Context() {
        static const wchar_t* className=L"PySLMNativeHiddenOpenGL";
        static std::once_flag once;
        std::call_once(once,[&]{WNDCLASSW wc{};wc.style=CS_OWNDC;wc.lpfnWndProc=DefWindowProcW;
            wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=className;
            if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
                throw std::runtime_error("Cannot register hidden OpenGL window");});
        try {
            window=CreateWindowW(className,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(!window)throw std::runtime_error("Cannot create hidden OpenGL window");
            dc=GetDC(window);PIXELFORMATDESCRIPTOR p{};p.nSize=sizeof(p);p.nVersion=1;
            p.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;p.iPixelType=PFD_TYPE_RGBA;
            p.cColorBits=32;p.cDepthBits=24;p.iLayerType=PFD_MAIN_PLANE;
            int format=ChoosePixelFormat(dc,&p);
            if(!format||!SetPixelFormat(dc,format,&p))throw std::runtime_error("Cannot select OpenGL pixel format");
            rc=wglCreateContext(dc);
            if(!rc||!wglMakeCurrent(dc,rc))throw std::runtime_error("Cannot activate OpenGL context");
        }catch(...){release();throw;}
    }
    void release() noexcept {
        if(rc){wglMakeCurrent(nullptr,nullptr);wglDeleteContext(rc);rc=nullptr;}
        if(dc&&window){ReleaseDC(window,dc);dc=nullptr;}
        if(window){DestroyWindow(window);window=nullptr;}
        wglMakeCurrent(previousDC,previousRC);
    }
    ~Context(){release();}
};
struct GL {
#define FN(ret,name,args) using name##Fn=ret(APIENTRY*)args;name##Fn name=loadGL<name##Fn>("gl" #name)
    FN(GLuint,CreateShader,(GLenum));FN(void,ShaderSource,(GLuint,GLsizei,const char* const*,const GLint*));
    FN(void,CompileShader,(GLuint));FN(void,GetShaderiv,(GLuint,GLenum,GLint*));
    FN(void,GetShaderInfoLog,(GLuint,GLsizei,GLsizei*,char*));FN(void,DeleteShader,(GLuint));
    FN(GLuint,CreateProgram,());FN(void,AttachShader,(GLuint,GLuint));FN(void,LinkProgram,(GLuint));
    FN(void,GetProgramiv,(GLuint,GLenum,GLint*));FN(void,GetProgramInfoLog,(GLuint,GLsizei,GLsizei*,char*));
    FN(void,UseProgram,(GLuint));FN(void,DeleteProgram,(GLuint));
    FN(GLint,GetUniformLocation,(GLuint,const char*));FN(void,UniformMatrix4fv,(GLint,GLsizei,GLboolean,const GLfloat*));
    FN(GLint,GetAttribLocation,(GLuint,const char*));FN(void,EnableVertexAttribArray,(GLuint));
    FN(void,VertexAttribPointer,(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*));
    FN(void,GenFramebuffers,(GLsizei,GLuint*));FN(void,BindFramebuffer,(GLenum,GLuint));
    FN(void,FramebufferTexture2D,(GLenum,GLenum,GLenum,GLuint,GLint));
    FN(void,GenRenderbuffers,(GLsizei,GLuint*));FN(void,BindRenderbuffer,(GLenum,GLuint));
    FN(void,RenderbufferStorage,(GLenum,GLenum,GLsizei,GLsizei));
    FN(void,FramebufferRenderbuffer,(GLenum,GLenum,GLenum,GLuint));
    FN(GLenum,CheckFramebufferStatus,(GLenum));FN(void,DeleteFramebuffers,(GLsizei,const GLuint*));
    FN(void,DeleteRenderbuffers,(GLsizei,const GLuint*));
#undef FN
    GLuint shader(GLenum type,const char* source) {
        GLuint s=CreateShader(type);ShaderSource(s,1,&source,nullptr);CompileShader(s);
        GLint ok=0;GetShaderiv(s,0x8B81,&ok);
        if(!ok){char message[2048]{};GetShaderInfoLog(s,sizeof(message),nullptr,message);DeleteShader(s);
            throw std::runtime_error(std::string("Height-map shader: ")+message);}return s;
    }
};
Bounds3 bounds(const Mesh& mesh) {
    if(mesh.vertices.empty())throw std::invalid_argument("Cannot project an empty mesh");
    const auto p=mesh.vertices.front();Bounds3 b{p.x,p.y,p.z,p.x,p.y,p.z};
    for(auto v:mesh.vertices){b[0]=std::min(b[0],v.x);b[1]=std::min(b[1],v.y);b[2]=std::min(b[2],v.z);
        b[3]=std::max(b[3],v.x);b[4]=std::max(b[4],v.y);b[5]=std::max(b[5],v.z);}return b;
}
}
std::string openGLRendererInfo() {
    std::lock_guard<std::mutex> lock(renderMutex);Context context;
    auto s=[](GLenum e){auto p=glGetString(e);return p?reinterpret_cast<const char*>(p):"unavailable";};
    return std::string(s(GL_VENDOR))+"; "+s(GL_RENDERER)+"; "+s(GL_VERSION);
}
HeightMap projectHeightMap(const Mesh& mesh,double resolution,bool flipDir,std::optional<Bounds3> bbox) {
    if(!(resolution>0)||!std::isfinite(resolution))throw std::invalid_argument("Height-map resolution must be positive");
    auto b=bbox?*bbox:bounds(mesh);
    for(auto value:b)if(!std::isfinite(value))throw std::invalid_argument("Height-map bounds must be finite");
    if(!(b[3]>b[0]&&b[4]>b[1]))throw std::invalid_argument("Height-map XY bounds must have positive extent");
    double rawWidth=(b[3]-b[0])/resolution,rawHeight=(b[4]-b[1])/resolution;
    if(!std::isfinite(rawWidth)||!std::isfinite(rawHeight)||rawWidth<1||rawHeight<1||rawWidth>32768||rawHeight>32768)throw std::invalid_argument("Height-map image dimensions are invalid");
    const auto w=static_cast<std::size_t>(rawWidth),h=static_cast<std::size_t>(rawHeight);
    if(!w||!h||w>32768||h>32768||w*h>268435456)throw std::invalid_argument("Height-map image dimensions are invalid");
    std::vector<float> vertices;vertices.reserve(mesh.vertices.size()*3);
    for(auto v:mesh.vertices){vertices.push_back(float(v.x));vertices.push_back(float(v.y));vertices.push_back(float(v.z));}
    std::vector<GLuint> indices;indices.reserve(mesh.faces.size()*3);
    for(auto f:mesh.faces)for(auto i:f){if(i>=mesh.vertices.size())throw std::out_of_range("Invalid triangle vertex");indices.push_back(i);}
    std::lock_guard<std::mutex> lock(renderMutex);Context context;GL gl;
    constexpr GLenum framebuffer=0x8D40,renderbuffer=0x8D41,colorAttachment=0x8CE0,depthAttachment=0x8D00;
    GLuint texture=0,fbo=0,depth=0,program=0,vs=0,fs=0;
    auto cleanup=[&]{if(program)gl.DeleteProgram(program);if(vs)gl.DeleteShader(vs);if(fs)gl.DeleteShader(fs);
        if(depth)gl.DeleteRenderbuffers(1,&depth);if(fbo)gl.DeleteFramebuffers(1,&fbo);if(texture)glDeleteTextures(1,&texture);};
    try {
        glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,0x8814,GLsizei(w),GLsizei(h),0,GL_RGBA,GL_FLOAT,nullptr);
        gl.GenFramebuffers(1,&fbo);gl.BindFramebuffer(framebuffer,fbo);
        gl.FramebufferTexture2D(framebuffer,colorAttachment,GL_TEXTURE_2D,texture,0);
        gl.GenRenderbuffers(1,&depth);gl.BindRenderbuffer(renderbuffer,depth);
        // Vispy RenderBuffer(format='depth') uses GL_DEPTH_COMPONENT16.
        gl.RenderbufferStorage(renderbuffer,0x81A5,GLsizei(w),GLsizei(h));
        gl.FramebufferRenderbuffer(framebuffer,depthAttachment,renderbuffer,depth);
        if(gl.CheckFramebufferStatus(framebuffer)!=0x8CD5)throw std::runtime_error("Incomplete floating-point framebuffer");
        const char* vert="uniform mat4 u_model,u_view,u_projection; attribute vec3 a_position,a_color; varying vec4 v_color; void main(){v_color=vec4(a_color,1.0);gl_Position=u_projection*u_view*u_model*vec4(a_position,1.0);}";
        const char* frag="varying vec4 v_color;void main(){gl_FragColor=vec4(v_color.z,v_color.z,v_color.z,1.0);}";
        vs=gl.shader(0x8B31,vert);fs=gl.shader(0x8B30,frag);program=gl.CreateProgram();
        gl.AttachShader(program,vs);gl.AttachShader(program,fs);gl.LinkProgram(program);
        GLint linked=0;gl.GetProgramiv(program,0x8B82,&linked);
        if(!linked){char log[2048]{};gl.GetProgramInfoLog(program,sizeof(log),nullptr,log);throw std::runtime_error(log);}
        gl.UseProgram(program);
        const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        float view[16];std::copy(identity,identity+16,view);
        if(!flipDir){view[5]=-1;view[10]=-1;view[13]=float(b[1]+b[4]);view[14]=float(b[2]+b[5]);}
        const float proj[16]={float(2/(b[3]-b[0])),0,0,0,0,float(2/(b[4]-b[1])),0,0,
            0,0,-0.0001f,0,float(-(b[3]+b[0])/(b[3]-b[0])),float(-(b[4]+b[1])/(b[4]-b[1])),0,1};
        gl.UniformMatrix4fv(gl.GetUniformLocation(program,"u_model"),1,GL_FALSE,identity);
        gl.UniformMatrix4fv(gl.GetUniformLocation(program,"u_view"),1,GL_FALSE,view);
        gl.UniformMatrix4fv(gl.GetUniformLocation(program,"u_projection"),1,GL_FALSE,proj);
        for(auto name:{"a_position","a_color"}){auto loc=gl.GetAttribLocation(program,name);
            gl.EnableVertexAttribArray(GLuint(loc));gl.VertexAttribPointer(GLuint(loc),3,GL_FLOAT,GL_FALSE,0,vertices.data());}
        glViewport(0,0,GLsizei(w),GLsizei(h));glClearColor(0,0,0,0);glClearDepth(1);
        glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);glDisable(GL_POLYGON_OFFSET_FILL);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glDrawElements(GL_TRIANGLES,GLsizei(indices.size()),GL_UNSIGNED_INT,indices.data());
        std::vector<float> pixels(w*h*4);glReadPixels(0,0,GLsizei(w),GLsizei(h),GL_RGBA,GL_FLOAT,pixels.data());
        const GLenum error=glGetError();if(error!=GL_NO_ERROR)throw std::runtime_error("Height-map OpenGL error "+std::to_string(error));
        HeightMap result;result.width=w;result.height=h;result.float32=true;result.values.resize(w*h);
        for(std::size_t row=0;row<h;++row)for(std::size_t col=0;col<w;++col)result.at(col,row)=pixels[((h-1-row)*w+col)*4+1];
        cleanup();return result;
    }catch(...){cleanup();throw;}
}
} // namespace pyslm::support
