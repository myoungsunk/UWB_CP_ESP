#line 1 "D:\\SLAM_bot\\artifacts\\UWB_MODE8_STAGE6_CONTINUOUS_RX_FEC_20261111\\anchor\\firmware\\ANCHOR_DUAL_DS_CIR\\uart_fragment.hpp"
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace dual8 {
constexpr unsigned UART_FRAGMENT_BYTES=256;
constexpr unsigned UART_FRAGMENT_MAX_DATA=96;
constexpr unsigned UART_FRAGMENT_MAX_PARITY=20;
constexpr unsigned UART_FRAGMENT_HEADER=16;
constexpr unsigned UART_FRAGMENT_END=255;
constexpr unsigned uartParityCount(unsigned data){
 return data+4<UART_FRAGMENT_MAX_PARITY?data+4:UART_FRAGMENT_MAX_PARITY;
}

// Systematic Cauchy code over GF(256), primitive polynomial 0x11d.
// Valid fragment CRCs locate erasures. Any data-count valid rows suffice.
class UartField {
 uint8_t logarithm_[256]{}, powers_[512]{};
public:
 UartField(){
  unsigned v=1;
  for(unsigned i=0;i<255;++i){
   powers_[i]=uint8_t(v);logarithm_[v]=uint8_t(i);
   v<<=1;if(v&256U)v^=0x11d;
  }
  for(unsigned i=255;i<512;++i)powers_[i]=powers_[i-255];
 }
 void add(uint8_t*parity,const uint8_t*data,unsigned n,unsigned dataIndex,unsigned parityIndex)const{
  const unsigned inverseLog=255-logarithm_[dataIndex^parityIndex];
  for(unsigned i=0;i<n;++i){
   const uint8_t v=data[i];
   if(v)parity[i]^=powers_[inverseLog+logarithm_[v]];
  }
 }
};
} // namespace dual8

