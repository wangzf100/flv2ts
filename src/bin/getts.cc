#include <iostream>
#include <string>
#include <stdlib.h>
#include <inttypes.h>
#include <aux/file_mapped_memory.hh>
#include <h264/avc_decoder_configuration_record.hh>

#include <flv/parser.hh>
#include <adts/header.hh>
#include <ts/packet.hh>
#include <aux/crc32.hh>
#include <h264/avc_sample.hh>

#include "ts_write.hh"

using namespace flv2ts;

int main(int argc, char** argv) {
  if(argc != 4) {
    std::cerr << "Usage: getts FLV_FILE HLS_INDEX_PREFIX SEQUNCE" << std::endl;
    return 1;
  }

  const std::string flv_path = argv[1];
  const std::string prefix = argv[2];
  const unsigned sequence = atoi(argv[3]);

  // read configuration
  h264::AVCDecoderConfigurationRecord conf;
  {
    aux::FileMappedMemory conf_mm((prefix + ".avc_conf").c_str());
    if(! conf_mm) {
      std::cerr << "Can't open file: " << prefix + ".avc_conf" << std::endl;
      return 1;
    }
    aux::ByteStream conf_in(conf_mm.ptr<uint8_t>(), conf_mm.size());
    if(! conf.parse(conf_in)) {
      std::cerr << "parse AVCDecoderConfigurationRecord failed" << std::endl;
      return 1;
    }
  }

  // read ts index
  uint32_t start_pos = 0;
  uint32_t end_pos = 0;
  {
    aux::FileMappedMemory index_mm((prefix + ".ts_idx").c_str());
    if(! index_mm) {
      std::cerr << "Can't open file: " << prefix + ".ts_idx" << std::endl;
      return 1;
    }
    
    if(sequence < 0 || sequence > index_mm.size() / sizeof(uint32_t)) {
      std::cerr << "out of range: sequence=" << sequence << std::endl;
      return 1;
    }

    start_pos = *index_mm.ptr<uint32_t>((sequence+0) * sizeof(uint32_t));
    end_pos   = *index_mm.ptr<uint32_t>((sequence+1) * sizeof(uint32_t));
  }

  // flv open
  aux::FileMappedMemory flv_mm(flv_path.c_str());
  if(! flv_mm) {
    std::cerr << "Can't open file: " << flv_path << " [" << start_pos << ".." << end_pos << "]" << std::endl;
    return 1;
  }
  
  flv::Parser flv(flv_mm, start_pos, end_pos);
  if(! flv) {
    std::cerr << "flv parser initialization failed" << std::endl;
    return 1;
  }

  // output ts
  std::ostream& ts_out = std::cout;
  tw_state state;
  
  // PAT/PMT(ts), SPS/PPS(h264)
  { 
    flv::Tag tag; // XXX:
    tag.timestamp = 0;
    tag.video.composition_time = 0;
    tag.video.frame_type = flv::VideoTag::FRAME_TYPE_INTER;

    std::string buf;
    write_ts_start(ts_out);
    to_storage_format_sps_pps(conf, buf); 
    write_video(state, tag, buf, ts_out);
  }
  
  for(;;) {
    flv::Tag tag;
    uint32_t prev_tag_size;
    if(! flv.parseTag(tag, prev_tag_size)) {
      std::cerr << "parse flv tag failed" << std::endl;
      return 1;
    }
    
    if(flv.eos()) {
      break;
    }

    switch(tag.type) {
    case flv::Tag::TYPE_AUDIO: {
      // audio
      if(tag.audio.sound_format != 10) { // 10=AAC
        std::cerr << "unsupported audio format: " << tag.audio.sound_format << std::endl;
        return 1;
      }
      if(tag.audio.aac_packet_type == 0) {
        // AudioSpecificConfig
        continue;
      }
      
      adts::Header adts = adts::Header::make_default(tag.audio.payload_size);
      char buf[7];
      adts.dump(buf, 7);
      
      std::string buf2;
      buf2.assign(buf, 7); // TODO: 既にADTSヘッダが付いているかのチェック
      buf2.append(reinterpret_cast<const char*>(tag.audio.payload), tag.audio.payload_size);
      
      write_audio(state, tag, buf2, ts_out);
      break;
    }
      
    case flv::Tag::TYPE_VIDEO: {
      // video
      if(tag.video.codec_id != 7) { // 7=AVC
        std::cerr << "unsupported video codec: " << tag.video.codec_id << std::endl;
        return 1;
      }

      switch(tag.video.avc_packet_type) {
      case 0: {
        // AVC sequence header
        break;
      }
      case 1: {
        // AVC NALU
        std::string buf;
        if(! to_storage_format(conf, tag.video.payload, tag.video.payload_size, buf)) {
          std::cerr << "to_strage_format() failed" << std::endl;
          return 1;
        }
        write_video(state, tag, buf, ts_out);
        
        break;
      }
      case 2: {
        // AVC end of sequence
        break;
      }
      default: {
        std::cerr << "unknown avc_packet_type: " << tag.video.avc_packet_type << std::endl;
        return 1;
      }
      }
    }

    default:
      break;
    }
  }
  
  return 0;
}
