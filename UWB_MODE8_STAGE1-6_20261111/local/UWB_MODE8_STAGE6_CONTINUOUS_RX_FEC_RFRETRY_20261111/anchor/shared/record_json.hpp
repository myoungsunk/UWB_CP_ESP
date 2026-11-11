#pragma once
#include "core.hpp"
#include <stdio.h>
#include <stdarg.h>
namespace dual8 {
constexpr size_t JSON_MAX=7000;
inline const char* firmwareId(bool anchor){
 return anchor?"D8_R6_ANCHOR_STAGE6_RX_FEC_RFRETRY_20261111":"D8_R4_SOURCE_20261111";
}
class Json {
 char* p_;size_t cap_;
public:
 size_t size=0;bool ok=true;
 Json(char*p,size_t cap):p_(p),cap_(cap){if(cap_)p_[0]=0;}
 void add(const char*fmt,...) __attribute__((format(printf,2,3))){if(!ok)return;va_list a;va_start(a,fmt);int n=vsnprintf(p_+size,cap_-size,fmt,a);va_end(a);if(n<0||size_t(n)>=cap_-size){ok=false;return;}size+=size_t(n);}
 void num(double v){if(isfinite(v))add("%.12g",v);else add("null");}
 void hex(const uint8_t*p,size_t n){add("\"");for(size_t k=0;k<n;++k)add("%02x",unsigned(p[k]));add("\"");}
};
inline void jsonRx(Json&j,const Rx&o){
 j.add("{\"ok\":%s,\"radio\":%u,\"type\":%u,\"hw\":%llu,\"raw\":%llu,\"esp_us\":%llu,\"arm_us\":%llu,\"status\":%llu,\"lde\":%s,\"fp_raw\":%u,\"nacc\":%u,\"pwr\":%u,\"f1\":%u,\"f2\":%u,\"f3\":%u,\"noise\":%u,\"ci\":%ld,\"diag_us\":%lu,\"rx_dbm\":",
 o.received?"true":"false",unsigned(o.radio),unsigned(o.type),(unsigned long long)o.hw,(unsigned long long)o.raw,(unsigned long long)o.esp_us,(unsigned long long)o.arm_us,(unsigned long long)o.status,o.lde?"true":"false",o.fp_raw,o.nacc,o.pwr,o.f1,o.f2,o.f3,o.noise,(long)o.ci,(unsigned long)o.diag_us);
 j.num(rxPower(o.pwr,o.nacc));j.add(",\"fp_dbm\":");j.num(fpPower(o.f1,o.f2,o.f3,o.nacc));j.add(",\"air\":");j.hex(o.air,o.received?AIR_BYTES:0);j.add("}");
}
inline void jsonTx(Json&j,const Tx&o){j.add("{\"attempted\":%s,\"ok\":%s,\"radio\":%u,\"type\":%u,\"requested\":%llu,\"planned\":%llu,\"actual\":%llu,\"status\":%llu,\"lead_us\":%ld,\"begin_us\":%llu,\"end_us\":%llu}",o.attempted?"true":"false",o.ok?"true":"false",o.radio,o.type,(unsigned long long)o.requested,(unsigned long long)o.planned,(unsigned long long)o.actual,(unsigned long long)o.status,(long)o.lead_us,(unsigned long long)o.begin_us,(unsigned long long)o.end_us);}
inline void jsonFrame(Json&j,const Frame&f,bool valid){uint8_t b[AIR_BYTES];if(valid&&encode(f,b))j.hex(b,AIR_BYTES);else j.add("null");}
inline bool serializeRecord(const Record&r,char*out,size_t capacity,size_t&bytes){
 Json j(out,capacity);
 j.add("{\"schema\":\"dual8.record.v1\",\"firmware\":\"%s\",\"role\":\"%s\",\"mode\":8,\"stage\":%u,\"session\":%lu,\"txn\":%lu,\"boot\":%lu,\"peer_boot\":%lu,\"config\":%lu,\"begin_us\":%llu,\"end_us\":%llu,\"complete\":%s,\"errors\":%lu,\"metadata_conflict\":%s,\"p1_tx_confirmed\":%s,\"resp_mask\":%u,\"period_us\":%lu,\"cir_hz\":%lu,\"baud\":%lu,\"read_order\":\"%s\",\"tx\":[",
 firmwareId(r.anchor),r.anchor?"anchor":"tag",r.cfg.stage,(unsigned long)r.session,(unsigned long)r.txn,(unsigned long)r.boot,(unsigned long)r.peer_boot,(unsigned long)r.config,(unsigned long long)r.begin_us,(unsigned long long)r.end_us,r.complete?"true":"false",(unsigned long)r.errors,r.metadata_conflict?"true":"false",r.p1_tx_confirmed?"true":"false",r.response_mask,(unsigned long)r.cfg.period_us,(unsigned long)r.cfg.cir_hz,(unsigned long)r.cfg.baud,r.txn&1?"B,A":"A,B");
 for(unsigned i=0;i<(r.anchor?3U:2U);++i){if(i)j.add(",");jsonTx(j,r.tx[i]);}j.add("],\"poll_rx\":[");
 if(!r.anchor){for(unsigned i=0;i<2;++i){if(i)j.add(",");jsonRx(j,r.poll[i]);}}
 j.add("],\"response_rx\":[");
 if(r.anchor){for(unsigned i=0;i<2;++i){if(i)j.add(",");jsonRx(j,r.responses[i]);}}
 if(r.anchor&&r.cfg.usesExtendedSchedule()){
  j.add("],\"response_trace\":[");
  for(unsigned i=0;i<2;++i){
   if(i)j.add(",");
   const auto& t=r.response_trace[i];
   j.add("{\"slot\":%u,\"status_or\":%llu,\"last_failure\":%llu,\"failed_reads\":%lu,\"rejected_frames\":%lu,"
    "\"recovered_reads\":%lu,\"first_read_issue\":%u,\"first_read_length\":%u}",
    i,(unsigned long long)t.status_or,(unsigned long long)t.last_failure,
    (unsigned long)t.failed_reads,(unsigned long)t.rejected_frames,(unsigned long)t.recovered_reads,
    unsigned(t.first_read_issue),unsigned(t.first_read_length));
  }
 }
 j.add("],\"probe_rx\":[");
 if(!r.anchor){for(unsigned s=0;s<4;++s){if(s)j.add(",");jsonRx(j,r.probe[s/2][s%2]);}}
 j.add("],\"frames\":[");
 jsonFrame(j,r.poll_frame,true);for(unsigned i=0;i<2;++i){j.add(",");jsonFrame(j,r.probe_frame[i],r.have_probe_frame[i]);}j.add("],\"ranges\":[");
 if(!r.anchor){for(unsigned i=0;i<2;++i){if(i)j.add(",");const auto&d=r.range[i];const auto&v=r.range_input[i];const auto&c=r.cal[i];
  j.add("{\"radio\":%u,\"math_valid\":%s,\"d_m\":",i,d.valid?"true":"false");j.num(d.range_m);j.add(",\"tof_ticks\":");j.num(d.tof_ticks);
  j.add(",\"r1\":%llu,\"d1\":%llu,\"r2\":%llu,\"d2\":%llu,\"six_ts\":[%llu,%llu,%llu,%llu,%llu,%llu],\"cal_id\":%lu,\"ant_tx\":%u,\"ant_rx\":%u,\"range_sigma_m\":",
  (unsigned long long)d.r1,(unsigned long long)d.d1,(unsigned long long)d.r2,(unsigned long long)d.d2,(unsigned long long)v.a_poll_tx,(unsigned long long)v.a_resp_rx,(unsigned long long)v.a_probe_tx,(unsigned long long)v.t_poll_rx,(unsigned long long)v.t_resp_tx,(unsigned long long)v.t_probe_rx,(unsigned long)c.id,c.tx,c.rx);
  j.num(c.range_sigma_m);j.add("}");
 }}
 j.add("],\"cir\":[");
 if(!r.anchor)for(unsigned s=0;s<4;++s){if(s)j.add(",");const auto&m=r.cir_meta[s];j.add("{\"slot\":%u,\"radio\":%u,\"probe\":%u,\"present\":%s,\"valid\":%s,\"repeat_equal\":%s,\"bytes\":%lu,\"crc32\":%lu,\"read_us\":%lu,\"repeat_us\":%lu}",s,s%2,s/2+1,m.present?"true":"false",m.valid?"true":"false",m.repeat_equal?"true":"false",(unsigned long)m.bytes,(unsigned long)m.crc,(unsigned long)m.read_us,(unsigned long)m.repeat_us);}
 j.add("],\"calibration\":[");
 for(unsigned i=0;i<(r.anchor?1U:2U);++i){if(i)j.add(",");const auto&c=r.cal[i];j.add("{\"radio\":%u,\"id\":%lu,\"tx\":%u,\"rx\":%u,\"range_sigma_m\":",i,(unsigned long)c.id,c.tx,c.rx);j.num(c.range_sigma_m);j.add("}");}
 if(r.cfg.rangesProbe2()){
  j.add("],\"%s\":[",r.anchor?"response2_rx":"response2_tx");
  for(unsigned i=0;i<2;++i){
   if(i)j.add(",");
   if(r.anchor)jsonRx(j,r.response2_rx[i]);
   else {
    const auto& t=r.response2_tx[i];
    j.add("{\"ok\":%s,\"radio\":%u,\"type\":%u,\"planned\":%llu,\"actual\":%llu,\"status\":%llu,\"lead_us\":%ld}",
          t.ok?"true":"false",unsigned(t.radio),unsigned(t.type),(unsigned long long)t.planned,
          (unsigned long long)t.actual,(unsigned long long)t.status,(long)t.lead_us);
   }
  }
  j.add("],\"poll2_frame\":");jsonFrame(j,r.poll2_frame,r.have_poll2_frame);
  if(r.anchor){j.add(",\"poll2_tx\":");jsonTx(j,r.poll2_tx);}
  else {
   j.add(",\"poll2_rx\":[");
   for(unsigned i=0;i<2;++i){
    if(i)j.add(",");
    const auto& o=r.poll2_rx[i];
    j.add("{\"ok\":%s,\"lde\":%s,\"radio\":%u,\"type\":%u,\"hw\":%llu,\"raw\":%llu,\"air_crc\":%lu}",
          o.received?"true":"false",o.lde?"true":"false",unsigned(o.radio),unsigned(o.type),
          (unsigned long long)o.hw,(unsigned long long)o.raw,(unsigned long)getLE(o.air+92,4));
   }
   j.add("]");
  }
  if(r.cfg.stage==6)j.add(",\"rx_deadline_latch\":%u,\"initial_poll_tx\":%llu,\"p1_retry\":%s,\"p1_first_errors\":%lu,\"p2_retry\":%s,\"p2_first_errors\":%lu",
   unsigned(r.rx_deadline_latch),(unsigned long long)r.initial_poll_tx,
   r.p1_retry?"true":"false",(unsigned long)r.p1_first_errors,
   r.p2_retry?"true":"false",(unsigned long)r.p2_first_errors);
  j.add(",\"power_formula\":\"DW16_RAW_NO_EMPIRICAL\"}");bytes=j.size;return j.ok&&bytes<=JSON_MAX;
 }
 j.add("],\"power_formula\":\"DW16_RAW_NO_EMPIRICAL\"}");bytes=j.size;return j.ok&&bytes<=JSON_MAX;
}
} // namespace dual8
