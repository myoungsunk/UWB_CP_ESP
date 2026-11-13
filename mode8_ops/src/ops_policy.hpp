#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
namespace m8ops {
enum class State : uint8_t { Idle, Prepared, Armed, Running, Draining, Stored, Fault };
inline const char* name(State s) {
  switch(s) {
    case State::Idle:return "IDLE"; case State::Prepared:return "PREPARED";
    case State::Armed:return "ARMED"; case State::Running:return "RUNNING";
    case State::Draining:return "DRAINING"; case State::Stored:return "STORED";
    default:return "FAULT";
  }
}
inline bool u32(const char* s,uint32_t& out) {
  if(!s||!*s)return false;
  uint64_t n=0;
  for(;*s;++s) {
    if(*s<'0'||*s>'9')return false;
    n=n*10+unsigned(*s-'0'); if(n>UINT32_MAX)return false;
  }
  out=uint32_t(n);return true;
}
inline int nibble(char c) {
  return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
}
inline bool unhex(const char* s,uint8_t* out,size_t n) {
  if(!s||strlen(s)!=2*n)return false;
  for(size_t i=0;i<n;++i) {
    int a=nibble(s[2*i]),b=nibble(s[2*i+1]);if(a<0||b<0)return false;
    out[i]=uint8_t(a*16+b);
  }
  return true;
}
inline void hex(const uint8_t* p,size_t n,char* out) {
  const char* d="0123456789abcdef";
  for(size_t i=0;i<n;++i){out[2*i]=d[p[i]>>4];out[2*i+1]=d[p[i]&15];}
  out[2*n]=0;
}
inline bool secureEqual(const uint8_t* a,const uint8_t* b,size_t n) {
  uint8_t v=0;for(size_t i=0;i<n;++i)v|=a[i]^b[i];return v==0;
}
struct Gate {
  State state=State::Idle;
  uint32_t session=0,count=0,config=0;
  uint64_t deadline=0;
  bool committed=false;
  bool maintenance(bool rf,bool output,bool unacked,bool data)const {
    return !rf&&!output&&!unacked&&!data&&(state==State::Idle);
  }
  bool prepare(uint32_t s,uint32_t n,uint32_t c,uint64_t now) {
    if(state==State::Prepared&&s==session&&n==count&&c==config)return true;
    if(state!=State::Idle||!s||!n||n>32||!c)return false;
    session=s;count=n;config=c;deadline=now+30000000ULL;
    committed=false;state=State::Prepared;return true;
  }
  bool arm(uint32_t s,uint32_t c,uint64_t now) {
    if((state==State::Armed||state==State::Running)&&session==s&&config==c)return true;
    if(state!=State::Prepared||s!=session||c!=config||now>=deadline)return false;
    committed=true;deadline=now+1000000ULL;state=State::Armed;return true;
  }
  bool due(uint64_t now)const{return state==State::Armed&&now>=deadline;}
  bool expire(uint64_t now) {
    if(state==State::Prepared&&now>=deadline){state=State::Idle;session=count=config=0;return true;}
    return false;
  }
};
// Conservative baseline ceiling: full CIR, all RF retry slots, 20% UART margin.
// This is a calculation, not a measured assurance of real-time operation.
struct Budget {uint32_t rf_min_us,uart_min_us,minimum_us;};
inline Budget budget(uint32_t baud) {
  const uint64_t raw=4ULL*3968+7000+24;
  uint64_t tx=raw;
  if(baud==2000000){uint64_t n=(raw+255)/256;tx=(n+20)*272+3*16;}
  uint32_t u=baud?uint32_t((tx*10*1200000ULL+baud-1)/baud):UINT32_MAX;
  // period >= period/2 + 3*28000 + 10000; also P2 gap requires >=148000.
  uint32_t r=188000;
  return {r,u,r>u?r:u};
}
}
