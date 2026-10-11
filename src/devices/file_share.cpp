#include "remoteos/resources.hpp"
#include "remoteos/protocol.hpp"
#include <cpp-httplib/httplib.h>
#include <filesystem>
#include <thread>
#include <sstream>
#include <algorithm>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
namespace remoteos::devices {
namespace {
std::string xml(const std::string& s){std::string r;for(char c:s){switch(c){case '&':r+="&amp;";break;case '<':r+="&lt;";break;case '>':r+="&gt;";break;case '"':r+="&quot;";break;case '\'':r+="&apos;";break;default:r+=c;}}return r;}
std::string escape(const std::string& s){static const char* hex="0123456789ABCDEF";std::string r;for(unsigned char c:s){if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')r+=c;else{r+='%';r+=hex[c>>4];r+=hex[c&15];}}return r;}
struct Fd{int value=-1;explicit Fd(int f):value(f){}~Fd(){if(value>=0)close(value);}};
}
struct FileShare::Impl {
  httplib::Server server;std::thread worker;int root=-1;unsigned port=0;std::string prefix;bool writable;
  Impl(const std::string& path,const std::string& capability,bool write):prefix("/"+capability+"/"),writable(write){
    if(capability.size()<32||capability.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::invalid_argument("Invalid file capability");
    root=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0)throw std::runtime_error("Shared folder is unavailable");
    server.new_task_queue=[] { return new httplib::ThreadPool(2,2,8); };
    server.set_keep_alive_max_count(1);server.set_read_timeout(15);server.set_write_timeout(15);server.set_payload_max_length(256ULL*1024*1024);
    server.CustomRoute("PROPFIND",".*",[](const auto&,auto&){});server.CustomRoute("MKCOL",".*",[](const auto&,auto&){});
    server.set_pre_routing_handler([this](const httplib::Request& req,httplib::Response& res){
      if(req.path.compare(0,prefix.size(),prefix)!=0){res.status=403;return httplib::Server::HandlerResponse::Handled;}
      try {
        auto path=req.path.substr(prefix.size());auto parts=components(path);
        if(req.method=="OPTIONS"){res.status=200;res.set_header("DAV","1");res.set_header("Allow",writable?"OPTIONS, GET, HEAD, PROPFIND, PUT, DELETE, MKCOL":"OPTIONS, GET, HEAD, PROPFIND");}
        else if(req.method=="GET"||req.method=="HEAD"||req.method=="PROPFIND")read(req,res,parts);
        else if(req.method=="MKCOL") {if(!writable){res.status=403;}else{auto parent=open_parent(parts);res.status=mkdirat(parent->value,parts.back().c_str(),0750)==0?201:409;}}
        else if(req.method=="DELETE"){if(!writable){res.status=403;}else{auto parent=open_parent(parts);struct stat st{};if(fstatat(parent->value,parts.back().c_str(),&st,AT_SYMLINK_NOFOLLOW)<0)res.status=404;else if(S_ISLNK(st.st_mode))res.status=403;else res.status=unlinkat(parent->value,parts.back().c_str(),S_ISDIR(st.st_mode)?AT_REMOVEDIR:0)==0?204:409;}}
        else if(req.method=="PUT")return httplib::Server::HandlerResponse::Unhandled;
        else res.status=405;
      }catch(...){res.status=403;}return httplib::Server::HandlerResponse::Handled;
    });
    server.Put(".*",[this](const httplib::Request& req,httplib::Response& res,const httplib::ContentReader& reader){
      if(!writable||req.path.compare(0,prefix.size(),prefix)){res.status=403;return;}
      try{auto parts=components(req.path.substr(prefix.size()));auto parent=open_parent(parts);
        struct stat existing{};
        if(fstatat(parent->value,parts.back().c_str(),&existing,AT_SYMLINK_NOFOLLOW)==0&&(!S_ISREG(existing.st_mode)||existing.st_nlink!=1))throw std::runtime_error("Unsafe target");
        const auto temporary=".remoteos-upload-"+remoteos::random_token(16);
        Fd fd(openat(parent->value,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0640));
        if(fd.value<0)throw std::runtime_error("Cannot create upload");
        const bool ok=reader([&](const char* data,size_t n){size_t pos=0;while(pos<n){auto count=::write(fd.value,data+pos,n-pos);if(count<=0)return false;pos+=count;}return true;});
        if(ok&&fsync(fd.value)==0&&renameat(parent->value,temporary.c_str(),parent->value,parts.back().c_str())==0){fsync(parent->value);res.status=201;}
        else{unlinkat(parent->value,temporary.c_str(),0);res.status=500;}
      }catch(...){res.status=403;}
    });
    port=server.bind_to_any_port("127.0.0.1");if(!port||port>65535){close(root);root=-1;throw std::runtime_error("File sharing bind failed");}
    worker=std::thread([this]{server.listen_after_bind();});server.wait_until_ready();
  }
  ~Impl(){server.stop();if(worker.joinable())worker.join();if(root>=0)close(root);}
  static std::vector<std::string> components(const std::string& p){
    if(p.size()>4096||p.find('\0')!=std::string::npos||p.find('\\')!=std::string::npos)throw std::runtime_error("Invalid path");
    std::vector<std::string> r;std::stringstream ss(p);std::string item;
    while(std::getline(ss,item,'/')){if(item.empty())continue;if(item=="."||item==".."||item.size()>255)throw std::runtime_error("Invalid component");r.push_back(item);}return r;
  }
  std::shared_ptr<Fd> open_parts(const std::vector<std::string>& parts){int fd=openat(root,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);for(size_t i=0;i<parts.size();++i){const int next=openat(fd,parts[i].c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|(i+1<parts.size()?O_DIRECTORY:0));close(fd);fd=next;if(fd<0)throw std::runtime_error("Unavailable path");}return std::make_shared<Fd>(fd);}
  std::shared_ptr<Fd> open_parent(std::vector<std::string> p){if(p.empty())throw std::runtime_error("Share root is protected");p.pop_back();return open_parts(p);}
  std::string property(const std::string& href,const std::string& name,const struct stat& st){return "<d:response><d:href>"+xml(href)+(S_ISDIR(st.st_mode)&&href.back()!='/'?"/":"")+"</d:href><d:propstat><d:prop><d:displayname>"+xml(name)+"</d:displayname><d:resourcetype>"+(S_ISDIR(st.st_mode)?"<d:collection/>":"")+"</d:resourcetype><d:getcontentlength>"+std::to_string(st.st_size)+"</d:getcontentlength></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";}
  void read(const httplib::Request& req,httplib::Response& res,const std::vector<std::string>& parts){
    auto fd=open_parts(parts);struct stat st{};if(fstat(fd->value,&st)<0||(!S_ISDIR(st.st_mode)&&!S_ISREG(st.st_mode))||(!S_ISDIR(st.st_mode)&&st.st_nlink>1)){res.status=403;return;}
    if(req.method=="PROPFIND"||S_ISDIR(st.st_mode)){
      std::string body=req.method=="PROPFIND"?"<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">"+property(req.path,parts.empty()?"SD card":parts.back(),st):"<!doctype html><meta charset=utf-8><title>RemoteOS shared files</title><style>body{background:#161616;color:#f4f4f4;font:16px sans-serif;padding:32px}a{color:#78a9ff}li{padding:8px}</style><h1>RemoteOS shared files</h1><p>Client SD card · "+std::string(writable?"Read and write":"Read only")+"</p><ul>";
      if(req.method!="PROPFIND"||req.get_header_value("Depth")!="0"){
        if(req.method=="PROPFIND"&&req.get_header_value("Depth")=="infinity"){res.status=403;return;}
        DIR* dir=fdopendir(dup(fd->value));if(!dir)throw std::runtime_error("Directory unavailable");
        size_t n=0;while(auto* e=readdir(dir)){const std::string name=e->d_name;if(name=="."||name=="..")continue;struct stat entry{};if(fstatat(fd->value,name.c_str(),&entry,AT_SYMLINK_NOFOLLOW)<0||S_ISLNK(entry.st_mode)||(!S_ISDIR(entry.st_mode)&&(!S_ISREG(entry.st_mode)||entry.st_nlink>1)))continue;
          const auto href=req.path+(req.path.back()=='/'?"":"/")+escape(name);
          body+=req.method=="PROPFIND"?property(href,name,entry):"<li><a href=\""+xml(href)+(S_ISDIR(entry.st_mode)?"/":"")+"\">"+xml(name)+"</a></li>";
          if(++n>=1000)break;
        }closedir(dir);
      }
      body+=req.method=="PROPFIND"?"</d:multistatus>":"</ul><p>For uploads, open this address with a WebDAV file manager. MOVE/LOCK are not supported.</p>";
      res.status=req.method=="PROPFIND"?207:200;res.set_header("Cache-Control","no-store");res.set_content(body,req.method=="PROPFIND"?"application/xml":"text/html");return;
    }
    if(st.st_size<0||uint64_t(st.st_size)>SIZE_MAX){res.status=413;return;}
    res.set_content_provider(static_cast<size_t>(st.st_size),"application/octet-stream",[fd](size_t offset,size_t length,httplib::DataSink& sink){char bytes[16384];auto n=pread(fd->value,bytes,std::min(length,sizeof(bytes)),offset);return n>0&&sink.write(bytes,n);});
  }
};
FileShare::FileShare(const std::string& root,const std::string& cap,bool writable):impl_(std::make_unique<Impl>(root,cap,writable)){}
FileShare::~FileShare()=default;unsigned FileShare::port()const{return impl_->port;}
}
