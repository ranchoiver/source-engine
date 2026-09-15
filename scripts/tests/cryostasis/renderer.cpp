#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "materialsystem/cryostasis_helpers.h"
#define APPROX_SRGB_ADAPTER 0
#define MAX(a,b) std::max(a,b)
#define MIN(a,b) std::min(a,b)
#define ARRAYSIZE(a) int(sizeof(a)/sizeof((a)[0]))
#define FILTER_KERNEL_SLOP 20
#define TEXTURE_GROUP_RENDER_TARGET "rt"
#define IMAGE_FORMAT_RGBA16161616F 1
#define VIEW_MAIN 0
static void require(bool ok, const char *message) { if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); } }
struct float2 {
    float x,y;
    float2(float a=0,float b=0):x(a),y(b){}
    float2 operator*(float s) const {return {x*s,y*s};}
    float2 operator*(float2 s) const {return {x*s.x,y*s.y};}
    float2 operator+(float2 s) const {return {x+s.x,y+s.y};}
    float2 operator-(float2 s) const {return {x-s.x,y-s.y};}
};
struct float4 {
    float x,y,z,w;
    float4(float a=0,float b=0,float c=0,float d=0):x(a),y(b),z(c),w(d){}
    float2 xy()const{return {x,y};} float2 zw()const{return {z,w};}
    float4 operator*(float s)const{return {x*s,y*s,z*s,w*s};}
    float4 operator+(float4 s)const{return {x+s.x,y+s.y,z+s.z,w+s.w};}
    float4 &operator+=(float4 s){*this=*this+s;return *this;}
};
float2 clamp(float2 p,float2 lo,float2 hi){return {std::max(lo.x,std::min(hi.x,p.x)),std::max(lo.y,std::min(hi.y,p.y))};}
float4 max(float4 p,float lo){return {std::max(p.x,lo),std::max(p.y,lo),std::max(p.z,lo),std::max(p.w,lo)};}
struct ITexture {
    int width=0,height=0,format=1; bool error=false; std::vector<float> pixels;
    ITexture(){} ITexture(int w,int h):width(w),height(h),pixels(w*h,-9000){}
    int GetActualWidth()const{return width;} int GetActualHeight()const{return height;}
    int GetImageFormat()const{return format;}
};
using sampler=ITexture*;
// Bilinear sampler, independently clamped at the physical texture edges.
static float sample(ITexture *t,float u,float v) {
    float x=std::max(0.f,std::min(float(t->width-1),u*t->width-.5f));
    float y=std::max(0.f,std::min(float(t->height-1),v*t->height-.5f));
    int ix=int(x),iy=int(y),jx=std::min(ix+1,t->width-1),jy=std::min(iy+1,t->height-1);
    auto at=[&](int a,int b){return t->pixels[b*t->width+a];};
    float a=at(ix,iy)*(1-(x-ix))+at(jx,iy)*(x-ix);
    float b=at(ix,jy)*(1-(x-ix))+at(jx,jy)*(x-ix);
    return a*(1-(y-iy))+b*(y-iy);
}
float4 tex2D(sampler t,float2 uv){float s=sample(t,uv.x,uv.y);return {s,s,s,1};}
// INSERT_SHADER
struct IMaterialVar {
    ITexture *texture=nullptr; float4 value;
    void SetTextureValue(ITexture *t){texture=t;}
    void SetVecValue(float x,float y,float z,float w){value={x,y,z,w};}
};
struct IMaterial {
    bool inverse=false,error=false; std::map<std::string,IMaterialVar> vars;
    IMaterialVar *FindVar(const char *name,bool *found,bool){*found=true;return &vars[name];}
};
static bool IsErrorTexture(ITexture *t){return !t||t->error;}
static bool IsErrorMaterial(IMaterial *m){return !m||m->error;}
struct Rect_t {int x,y,width,height;};
struct Context {
    ITexture *target=nullptr; int width=0,height=0; int pushes=0,draws=0,copies=0; Rect_t copied={};
    std::vector<float> lastInverse; bool reject=false;
    void PushRenderTargetAndViewport(){++pushes;}
    void PopRenderTargetAndViewport(){--pushes;}
    void GetViewport(int &x,int &y,int &w,int &h){x=3;y=7;w=width;h=height;}
    void CopyRenderTargetToTextureEx(ITexture *t,int,Rect_t *src,void*){++copies;copied=*src;t->pixels.assign(t->width*t->height,.125f);}
    void DrawScreenSpaceRectangle(IMaterial *m,int,int,int w,int h,float x0,float y0,float x1,float y1,int sw,int sh) {
        ++draws; require(w==width && h==height,"draw/viewport mismatch");
        ITexture *input=m->vars["$basetexture"].texture;
        require(target!=input,"render feedback loop");
        BaseTextureSampler=input;
        CryostasisBlurParams=m->vars["$cryostasisBlurParams"].value;
        CryostasisBlurBounds=m->vars["$cryostasisBlurBounds"].value;
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            // Source's DrawScreenSpaceRectangle maps the given endpoints to pixel centers.
            float u=(x0+.5f+(w>1?x*(x1-x0)/(w-1):0))/sw;
            float v=(y0+.5f+(h>1?y*(y1-y0)/(h-1):0))/sh;
            float result;
            if(m->inverse) {
                // Boundary stand-in: inverse shader is compiled separately with FXC.
                float c=sample(input,u,v);
                result=c/std::max(1-c,.1f)*7.806838f;
            } else result=BlurShader(PS_INPUT{{u,v}}).x;
            require(std::isfinite(result)&&result>=0,"stale border, NaN, or negative color");
            target->pixels[y*target->width+x]=result;
        }
        if(m->inverse) lastInverse=target->pixels;
    }
} context;
using IMatRenderContext=Context;
struct Materials {
    std::map<std::string,ITexture> textures;
    ITexture *FindTexture(const char *s,const char*){return &textures.at(s);}
} systemMaterials;
static Materials *materials=&systemMaterials;
struct CMatRenderContextPtr {
    explicit CMatRenderContextPtr(Materials*){} Context *operator->(){return &context;}
};
static IMaterial inverseMaterial,blurMaterial;
static IMaterial *GetCryostasisMagicHDRInverseMaterial(){return &inverseMaterial;}
static IMaterial *GetCryostasisMagicHDRBlurMaterial(){return &blurMaterial;}
static bool supported=true,s_bCryostasisPostProcessActive=true;
static int currentView=VIEW_MAIN;
static bool CanRunCryostasisPostProcess(){return supported;}
static int CurrentViewID(){return currentView;}
static const char *s_pCryostasisBloomTextureNames[]={"b0","b1","b2","b3","b4","b5","b6"};
static void SetRenderTargetAndViewPort(ITexture *t,int w,int h){context.target=t;context.width=w;context.height=h;}
// INSERT_PRODUCTION

// Reference uses separate, tightly allocated images as in upstream MagicHDR.
// Gaussian weights are computed from sigma=sqrt(13)*.5, not copied from FXC.
static ITexture referencePass(ITexture &input,int w,int h,float dx,float dy) {
    ITexture output(w,h); double weights[13],total=0;
    for(int j=-6;j<=6;++j){weights[j+6]=std::exp(-j*j/(2*3.25));total+=weights[j+6];}
    for(int y=0;y<h;++y) for(int x=0;x<w;++x){
        double sum=0;
        for(int j=-6;j<=6;++j) sum+=sample(&input,(x+.5f)/w+j*dx,(y+.5f)/h+j*dy)*weights[j+6]/total;
        output.pixels[y*w+x]=float(sum);
    }
    return output;
}
static void setup(int w,int h) {
    systemMaterials.textures.clear();
    auto &t=systemMaterials.textures;
    t["_rt_FullFrameFB"]=ITexture(w*4+1,h*4+1);
    t["_rt_CryostasisTemp"]=ITexture(w,h);
    t["_rt_CryostasisDepth"]=ITexture(w*4+1,h*4+1);
    for(const char *s:s_pCryostasisBloomTextureNames)t[s]=ITexture(w,h);
    inverseMaterial.inverse=true; inverseMaterial.error=blurMaterial.error=false;
    context=Context();supported=true;currentView=VIEW_MAIN;s_bCryostasisPostProcessActive=true;
}
int main(){
    int cases=0;
    for(auto size:std::vector<std::pair<int,int>>{{1,1},{3,2},{17,9},{64,64},{341,192},{480,270}}){
        int w=size.first,h=size.second;
        for(int pattern=0;pattern<3;++pattern){
            setup(w,h); auto &src=systemMaterials.textures["_rt_FullFrameFB"];
            for(int y=0;y<src.height;++y)for(int x=0;x<src.width;++x)
                src.pixels[y*src.width+x]=pattern==0?.7f:pattern==1?float(x+y)/(src.width+src.height):((x*13+y*7)%37)/40.f;
            require(GenerateCryostasisMagicHDRBloomTextures(&context),"generation failed");
            require(context.pushes==0 && context.draws==15,"unbalanced state or pass count");
            // Independently verify inverse source coordinates at representative pixels.
            for(int x:std::vector<int>{0,w/2,w-1}){
                float c=sample(&src,(x+.5f)/w,.5f/h);
                require(std::abs(context.lastInverse[x]-c/std::max(1-c,.1f)*7.806838f)<.001f,"inverse pixel centers drift");
            }
            ITexture prev(w,h);prev.pixels=context.lastInverse;
            for(int i=0;i<7;++i){
                float scale=float(1<<i);int aw=std::max(1,w/(1<<i)),ah=std::max(1,h/(1<<i));
                ITexture horizontal=referencePass(prev,w,h,scale/w,0);
                prev=referencePass(horizontal,aw,ah,0,scale/h);
                auto &actual=systemMaterials.textures[s_pCryostasisBloomTextureNames[i]];
                for(int y=0;y<ah;++y)for(int x=0;x<aw;++x)
                    require(std::abs(actual.pixels[y*w+x]-prev.pixels[y*aw+x])<.0007f,"bloom differs from independent tightly sized reference");
                // Check the final shader's region mapping, including edge/out-of-range UVs.
                float region[4];Cryostasis::BloomRegion(w,h,i,region);
                for(float u:std::vector<float>{-.02f,0,.25f,.5f,1,1.02f}){
                    float2 uv=clamp(float2(u,u)*float2(region[0],region[1]),{region[2],region[3]},{region[0]-region[2],region[1]-region[3]});
                    require(std::abs(sample(&actual,uv.x,uv.y)-sample(&prev,u,u))<.0008f,"final bloom region mismatch");
                }
            }
            ++cases;
        }
    }
    setup(17,9);
    auto &depth=systemMaterials.textures["_rt_CryostasisDepth"];
    context.width=55;context.height=31;
    InvalidateCryostasisDepthTexture(); require(!s_bCryostasisDepthValid,"stale view depth");
    CaptureCryostasisDepthTexture();require(s_bCryostasisDepthValid&&context.copies==1,"missing main depth capture");
    require(context.copied.x==3&&context.copied.y==7&&context.copied.width==55&&context.copied.height==31,"wrong depth viewport");
    currentView=1; CaptureCryostasisDepthTexture();require(context.copies==1,"secondary view overwrites main depth");
    require(depth.pixels[0]==.125f,"private depth overwritten");
    InvalidateCryostasisDepthTexture();currentView=VIEW_MAIN;depth.error=true;
    CaptureCryostasisDepthTexture();require(!s_bCryostasisDepthValid,"failed depth accepted");
    depth.error=false;s_bCryostasisPostProcessActive=false;
    CaptureCryostasisDepthTexture();require(context.copies==1,"inactive preset copies depth");
    s_bCryostasisPostProcessActive=true;supported=false;
    CaptureCryostasisDepthTexture();require(context.copies==1,"unsupported preset copies depth");
    require(!GenerateCryostasisMagicHDRBloomTextures(&context),"unsupported bloom enabled");
    supported=true;blurMaterial.error=true;
    require(!GenerateCryostasisMagicHDRBloomTextures(&context)&&context.pushes==0,"error material accepted");
    blurMaterial.error=false;systemMaterials.textures["b2"].format=0;
    require(!GenerateCryostasisMagicHDRBloomTextures(&context),"clipped non-float RT accepted");
    systemMaterials.textures["b2"].format=1;systemMaterials.textures["b2"].width++;
    require(!GenerateCryostasisMagicHDRBloomTextures(&context),"inconsistent RT size accepted");
    std::printf("PASS: %d bloom image cases (126 levels), source centers, region edges, depth lifetime/viewport, and failure paths\n",cases);
}
