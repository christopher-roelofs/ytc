// Option B POC — play a YouTube video on the device's native YouTube app via Cast.
// Launches the YouTube Cast receiver (appId 233637DE), then drives it over the
// YouTube MDX namespace (getMdxSessionStatus -> screenId, then flingVideo).
//
// Build: g++ -std=c++17 tools/yt_cast_poc.cpp -o /tmp/yt_cast_poc -lssl -lcrypto
// Run:   /tmp/yt_cast_poc <ip> <videoId>
#include "../third_party/json.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <string>
using json = nlohmann::json;

static void put_varint(std::string& o, uint64_t v){ do{uint8_t b=v&0x7f; v>>=7; if(v)b|=0x80; o+=(char)b;}while(v); }
static void put_str(std::string& o,int f,const std::string& s){ o+=(char)((f<<3)|2); put_varint(o,s.size()); o+=s; }
static void put_var(std::string& o,int f,uint64_t v){ o+=(char)((f<<3)|0); put_varint(o,v); }
static std::string cast_msg(const std::string& src,const std::string& dst,const std::string& ns,const std::string& pl){
    std::string m; put_var(m,1,0); put_str(m,2,src); put_str(m,3,dst); put_str(m,4,ns); put_var(m,5,0); put_str(m,6,pl); return m; }
static uint64_t get_var(const std::string& b,size_t& i){ uint64_t v=0;int sh=0; while(i<b.size()){uint8_t c=b[i++]; v|=(uint64_t)(c&0x7f)<<sh; if(!(c&0x80))break; sh+=7;} return v; }
struct Parsed{ std::string ns,payload,source; };
static Parsed parse(const std::string& b){ Parsed p; size_t i=0;
    while(i<b.size()){ uint64_t tag=get_var(b,i); int f=tag>>3,wt=tag&7;
        if(wt==0) get_var(b,i);
        else if(wt==2){ uint64_t l=get_var(b,i); std::string s=b.substr(i,l); i+=l;
            if(f==2)p.source=s; else if(f==4)p.ns=s; else if(f==6)p.payload=s; }
        else break; }
    return p; }

static SSL* g=nullptr;
static void die(const char* m){ std::fprintf(stderr,"FATAL: %s\n",m); exit(1); }
static void send_msg(const std::string& src,const std::string& dst,const std::string& ns,const std::string& pl){
    std::string m=cast_msg(src,dst,ns,pl); uint32_t len=htonl((uint32_t)m.size());
    std::string fr((char*)&len,4); fr+=m; size_t off=0;
    while(off<fr.size()){ int n=SSL_write(g,fr.data()+off,fr.size()-off); if(n<=0)die("write"); off+=n; }
    std::fprintf(stderr,">> [%s] %s\n", ns.c_str(), pl.c_str());
}
static bool read_frame(std::string& out){
    char lb[4]; int got=0; while(got<4){int n=SSL_read(g,lb+got,4-got); if(n<=0)return false; got+=n;}
    uint32_t len=ntohl(*(uint32_t*)lb); out.clear(); out.resize(len); size_t off=0;
    while(off<len){int n=SSL_read(g,&out[off],len-off); if(n<=0)return false; off+=n;} return true;
}

int main(int argc,char** argv){
    if(argc<3){ std::fprintf(stderr,"usage: %s <ip> <videoId>\n",argv[0]); return 2; }
    std::string ip=argv[1], vid=argv[2];
    addrinfo h{},*res; h.ai_family=AF_INET; h.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(ip.c_str(),"8009",&h,&res)) die("getaddrinfo");
    int fd=socket(res->ai_family,res->ai_socktype,0);
    if(connect(fd,res->ai_addr,res->ai_addrlen)) die("connect"); freeaddrinfo(res);
    timeval tv{8,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    SSL_library_init(); SSL_CTX* ctx=SSL_CTX_new(TLS_client_method()); g=SSL_new(ctx); SSL_set_fd(g,fd);
    if(SSL_connect(g)!=1){ ERR_print_errors_fp(stderr); die("SSL_connect"); }
    std::fprintf(stderr,"TLS connected to %s:8009\n", ip.c_str());

    const std::string CONN="urn:x-cast:com.google.cast.tp.connection";
    const std::string HEART="urn:x-cast:com.google.cast.tp.heartbeat";
    const std::string RECV="urn:x-cast:com.google.cast.receiver";
    const std::string MDX="urn:x-cast:com.google.youtube.mdx";
    const std::string SRC="sender-0";

    send_msg(SRC,"receiver-0",CONN,R"({"type":"CONNECT"})");
    send_msg(SRC,"receiver-0",RECV,R"({"type":"LAUNCH","appId":"233637DE","requestId":1})");

    std::string transport; bool connected=false, flung=false; int req=2;
    for(int it=0; it<60; ++it){
        std::string raw; if(!read_frame(raw)){ std::fprintf(stderr,"(timeout/closed)\n"); break; }
        Parsed p=parse(raw); json j=json::parse(p.payload,nullptr,false);
        std::string type = j.is_discarded()? "" : j.value("type","");
        if(p.ns==HEART){ if(type=="PING") send_msg(SRC,p.source.empty()?"receiver-0":p.source,HEART,R"({"type":"PONG"})"); continue; }
        std::fprintf(stderr,"<< [%s] %s\n", p.ns.c_str(), p.payload.c_str());
        if(p.ns==RECV && type=="RECEIVER_STATUS" && transport.empty()){
            for(auto& app: j.value("status",json::object()).value("applications",json::array()))
                if(app.value("appId","")=="233637DE"){ transport=app.value("transportId","");
                    std::fprintf(stderr,"== YouTube launched, transportId=%s\n",transport.c_str()); }
            if(!transport.empty() && !connected){
                send_msg(SRC,transport,CONN,R"({"type":"CONNECT"})");
                // Ask the YouTube receiver for its MDX session (gives screenId).
                send_msg(SRC,transport,MDX,R"({"type":"getMdxSessionStatus"})");
                connected=true;
            }
        }
        if(p.ns==MDX){
            // On any MDX status, try flinging the video (single-video playback).
            if(!flung && transport.size()){
                json fling={ {"type","flingVideo"}, {"data",{ {"videoId",vid}, {"currentTime",0} }} };
                send_msg(SRC,transport,MDX,fling.dump());
                flung=true;
            }
            if(type=="mdxSessionStatus"){
                auto d=j.value("data",json::object());
                std::fprintf(stderr,"== screenId=%s deviceId=%s\n",
                    d.value("screenId","").c_str(), d.value("deviceId","").c_str());
            }
        }
    }
    std::fprintf(stderr, flung? "flingVideo sent — check the TV\n" : "did not fling\n");
    return flung?0:1;
}
