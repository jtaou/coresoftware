/*!
 * \file MicromegasRawDataEvaluation.cc
 * \author Hugo Pereira Da Costa <hugo.pereira-da-costa@cea.fr>
 */

#include "MicromegasRawDataEvaluation.h"
#include "MicromegasDefs.h"

#include <Event/Event.h>
#include <Event/EventTypes.h>
#include <Event/packet.h>

#include <fun4all/Fun4AllReturnCodes.h>

#include <phool/PHCompositeNode.h>
#include <phool/getClass.h>

#include <TFile.h>
#include <TH1.h>
#include <TH2.h>
#include <TProfile.h>

#include <cassert>
#include <fstream>
#include <list>
#include <memory>
#include <set>

namespace
{
  // streamer for lists
  template <class T>
  std::ostream& operator<<(std::ostream& out, const std::list<T>& list)
  {
    if (list.empty())
    {
      out << "{}";
    }
    else
    {
      out << "{ ";
      bool first = true;
      for (const auto& value : list)
      {
        if (!first)
        {
          out << ", ";
        }
        out << value;
        first = false;
      }
      out << " }";
    }
    return out;
  }

  // get the difference between two BCO.
  template<class T>
  inline static constexpr T get_bco_diff( const T& first, const T& second )
  { return first < second ? (second-first):(first-second); }

}

//_________________________________________________________
void MicromegasRawDataEvaluation::Waveform::copy_from(const MicromegasRawDataEvaluation::Sample& sample)
{
  packet_id = sample.packet_id;
  lvl1_bco = sample.lvl1_bco;
  fee_bco = sample.fee_bco;
  checksum = sample.checksum;
  checksum_error = sample.checksum_error;
  fee_id = sample.fee_id;
  layer = sample.layer;
  tile = sample.tile;
  sampa_address = sample.sampa_address;
  sampa_channel = sample.sampa_channel;
  channel = sample.channel;
  strip = sample.strip;
  sample_max = sample.sample;
  adc_max = sample.adc;
  pedestal = sample.pedestal;
  rms = sample.rms;
}

//_________________________________________________________
void MicromegasRawDataEvaluation::Container::Reset()
{
  samples.clear();
  waveforms.clear();
  taggers.clear();
}

//_________________________________________________________
MicromegasRawDataEvaluation::MicromegasRawDataEvaluation(const std::string& name)
  : SubsysReco(name)
{
}

//_____________________________________________________________________
int MicromegasRawDataEvaluation::Init(PHCompositeNode* /*topNode*/)
{
  // read calibrations
  m_calibration_data.read(m_calibration_filename);

  m_evaluation_file.reset(new TFile(m_evaluation_filename.c_str(), "RECREATE"));
  m_evaluation_tree = new TTree("T", "T");
  m_container = new Container;
  m_evaluation_tree->Branch("Event", &m_container);
  return Fun4AllReturnCodes::EVENT_OK;
}

//____________________________________________________________________________..
int MicromegasRawDataEvaluation::InitRun(PHCompositeNode* /*topNode*/)
{
  return Fun4AllReturnCodes::EVENT_OK;
}

//___________________________________________________________________________
int MicromegasRawDataEvaluation::process_event(PHCompositeNode* topNode)
{
  // load relevant nodes
  // PRDF node
  auto event = findNode::getClass<Event>(topNode, "PRDF");
  assert(event);

  // check event type
  if (event->getEvtType() >= 8)
  {
    return Fun4AllReturnCodes::DISCARDEVENT;
  }

  m_container->Reset();

  // temporary storage for samples and waveforms, sorted by lvl1 bco
  std::multimap<uint64_t, Sample> sample_map;
  std::multimap<uint64_t, Waveform> waveform_map;

  // loop over TPOT packets
  for (const auto& packet_id : MicromegasDefs::m_packet_ids)
  {
    std::unique_ptr<Packet> packet(event->getPacket(packet_id));
    if (!packet)
    {
      // no data
      if (Verbosity() > 1)
      {
        std::cout << "MicromegasRawDataEvaluation::process_event - packet " << packet_id << " not found." << std::endl;
      }
      continue;
    }

    // taggers
    int n_tagger = packet->lValue(0, "N_TAGGER");

    // store tagged lvl1 bcos into a vector
    using bco_list_t = std::list<uint64_t>;
    bco_list_t main_bco_list;
    for (int t = 0; t < n_tagger; t++)
    {
      TaggerInformation tagger;
      tagger.packet_id = packet_id;
      tagger.tagger_type = (uint16_t) (packet->lValue(t, "TAGGER_TYPE"));
      tagger.is_lvl1 = static_cast<uint8_t>(packet->lValue(t, "IS_LEVEL1_TRIGGER"));
      tagger.is_endat = static_cast<uint8_t>(packet->lValue(t, "IS_ENDAT"));
      tagger.bco = static_cast<uint64_t>(packet->lValue(t, "BCO"));
      tagger.last_bco = static_cast<uint64_t>(packet->lValue(t, "LAST_BCO"));
      tagger.lvl1_count = static_cast<uint32_t>(packet->lValue(t, "LEVEL1_COUNT"));
      tagger.endat_count = static_cast<uint32_t>(packet->lValue(t, "ENDAT_COUNT"));

      if (m_flags & EvalTagger)
      {
        m_container->taggers.push_back(tagger);
      }

      if (tagger.is_lvl1 && (m_flags & (EvalSample | EvalWaveform)))
      {
        // store bco
        main_bco_list.push_back(tagger.bco);
      }
    }

    // get number of datasets (also call waveforms)
    const auto n_waveform = packet->iValue(0, "NR_WF");

    if (Verbosity())
    {
      std::cout << "MicromegasRawDataEvaluation::process_event -"
                << " packet: " << packet_id
                << " taggers: " << n_tagger
                << " n_lvl1_bco: " << main_bco_list.size()
                << " n_waveform: " << n_waveform
                << std::endl;

      if (!main_bco_list.empty())
      {
        std::cout << "MicromegasRawDataEvaluation::process_event -"
                  << " packet: " << packet_id
                  << " bco: " << std::hex << main_bco_list << std::dec
                  << std::endl;
      }
    }

    if (m_flags & (EvalSample | EvalWaveform))
    {
      // store available bco list for each fee
      std::map<unsigned short, bco_list_t> bco_list_map;

      // keep track of orphans
      using fee_pair_t = std::pair<unsigned int, unsigned int>;
      std::set<fee_pair_t> orphans;

      for (int iwf = 0; iwf < n_waveform; ++iwf)
      {
        // create running sample, assign packet, fee, layer and tile id
        Sample sample;
        sample.packet_id = packet_id;
        sample.fee_id = packet->iValue(iwf, "FEE");
        const auto hitsetkey = m_mapping.get_hitsetkey(sample.fee_id);
        sample.layer = TrkrDefs::getLayer( hitsetkey );
        sample.tile = MicromegasDefs::getTileId( hitsetkey );

        // get channel
        sample.channel = packet->iValue( iwf, "CHANNEL" );

        // bound check
        if( sample.channel >= MicromegasDefs::m_nchannels_fee )
        {
          if( Verbosity() )
          { std::cout << "MicromegasRawDataEvaluation::process_event - invalid channel: " << sample.channel << std::endl; }
          continue;
        }

        // beam crossing
        sample.fee_bco = static_cast<uint32_t>(packet->iValue(iwf, "BCO"));
        sample.lvl1_bco = 0;
        sample.lvl1_bco_masked = 0;

        // find bco matching map corresponding to fee
        auto& bco_matching_pair = m_fee_bco_matching_map[sample.fee_id];

        // find matching lvl1 bco
        static constexpr unsigned int max_fee_bco_diff = 10;
        if( get_bco_diff(sample.fee_bco,bco_matching_pair.first) < max_fee_bco_diff )
        {
          sample.lvl1_bco = bco_matching_pair.second;
          sample.lvl1_bco_masked = ( bco_matching_pair.second & 0xFFFFFU );

        } else {

          // find bco list corresponding to fee, insert main list if not found
          auto bco_list_iter = bco_list_map.lower_bound(sample.fee_id);
          if (bco_list_iter == bco_list_map.end() || sample.fee_id < bco_list_iter->first)
          {
            bco_list_iter = bco_list_map.insert(bco_list_iter, std::make_pair(sample.fee_id, main_bco_list));
          }

          // get local reference to fee's bco list
          auto& bco_list = bco_list_iter->second;
          if (!bco_list.empty())
          {
            if (Verbosity())
            {
              std::cout << "MicromegasRawDataEvaluation::process_event -"
                        << " fee_id: " << sample.fee_id
                        << " fee_bco: 0x" << std::hex << sample.fee_bco
                        << " gtm_bco: 0x" << bco_list.front()
                        << std::dec
                        << std::endl;
            }

            // fee_bco is new. Assume it corresponds to the first available lvl1 bco
            // update running fee_bco and lvl1_bco pair accordingly
            const auto lvl1_bco = bco_list.front();
            bco_matching_pair.first = sample.fee_bco;
            bco_matching_pair.second = lvl1_bco;
            sample.lvl1_bco = lvl1_bco;
            sample.lvl1_bco_masked = ( lvl1_bco & 0xFFFFFU );

            // remove bco from running list
            bco_list.pop_front();
          }
          else
          {
            if (Verbosity() && orphans.insert(std::make_pair(sample.fee_id, sample.fee_bco)).second)
            {
              std::cout << "MicromegasRawDataEvaluation::process_event -"
                        << " fee_id: " << sample.fee_id
                        << " fee_bco: 0x" << std::hex << sample.fee_bco << std::dec
                        << " gtm_bco: none"
                        << std::endl;
            }
          }
        }

        // checksum and checksum error
        sample.checksum = packet->iValue(iwf, "CHECKSUM");
        sample.checksum_error = packet->iValue(iwf, "CHECKSUMERROR");

        // increment bco map
        ++m_bco_map[sample.lvl1_bco];

        // channel, sampa_channel, sampa address and strip
        sample.sampa_address = packet->iValue( iwf, "SAMPAADDRESS" );
        sample.sampa_channel = packet->iValue( iwf, "SAMPACHANNEL" );
        sample.strip = m_mapping.get_physical_strip(sample.fee_id, sample.channel);

        // get channel rms and pedestal from calibration data
        const double pedestal = m_calibration_data.get_pedestal(sample.fee_id, sample.channel);
        const double rms = m_calibration_data.get_rms(sample.fee_id, sample.channel);
        sample.pedestal = pedestal;
        sample.rms = rms;

        // get number of samples and loop
        const unsigned short samples = packet->iValue(iwf, "SAMPLES");
        if (Verbosity() > 1)
        {
          std::cout << "MicromegasRawDataEvaluation::process_event -"
                    << " fee: " << sample.fee_id
                    << " tile: " << sample.tile
                    << " layer: " << sample.layer
                    << " tile: " << sample.tile
                    << " lvl1_bco: " << sample.lvl1_bco
                    << " fee_bco: " << sample.fee_bco
                    << " error: " << sample.checksum_error
                    << " channel: " << sample.channel
                    << " strip: " << sample.strip
                    << " samples: " << samples
                    << std::endl;
        }

        Sample sample_max;
        for (unsigned short is = 0; is < std::min<unsigned short>(samples, 1024); ++is)
        {
          // assign sample id and corresponding adc, save copy in container
          const uint16_t adc = packet->iValue(iwf, is);
          if (adc == MicromegasDefs::m_adc_invalid)
          {
            continue;
          }
          sample.sample = is;
          sample.adc = adc;
          sample_map.emplace(sample.lvl1_bco, sample);

          if (sample.adc > sample_max.adc)
          {
            sample_max = sample;
          }
        }

        if (m_flags & EvalWaveform)
        {
          Waveform waveform(sample_max);

          waveform.is_signal =
              rms > 0 &&
              waveform.adc_max >= m_min_adc &&
              waveform.sample_max >= m_sample_min &&
              waveform.sample_max < m_sample_max &&
              waveform.adc_max > pedestal + m_n_sigma * rms;

          waveform_map.emplace(waveform.lvl1_bco, waveform);
        }
      }
    }
  }

  // copy all samples and waveform to container
  if (m_flags & EvalSample)
  {
    for (auto&& [lvl_bco, sample] : sample_map)
    // clang-tidy suggests the std::move has no effect (trivially copyable type)
    //    { m_container->samples.push_back(std::move(sample)); }
    {
      m_container->samples.push_back(sample);
    }
  }

  if (m_flags & EvalWaveform)
  {
    for (auto&& [lvl1_bco, waveform] : waveform_map)
    // clang-tidy suggests the std::move has no effect (trivially copyable type)
    //    { m_container->waveforms.push_back(std::move(waveform)); }
    {
      m_container->waveforms.push_back(waveform);
    }
  }

  // fill evaluation tree
  m_evaluation_tree->Fill();

  return Fun4AllReturnCodes::EVENT_OK;
}

//_____________________________________________________________________
int MicromegasRawDataEvaluation::End(PHCompositeNode* /*topNode*/)
{
  if (m_evaluation_file && m_evaluation_tree)
  {
    m_evaluation_file->cd();
    m_evaluation_tree->Write();
    m_evaluation_file->Close();
  }

  // print bco map
  if (Verbosity())
  {
    for (const auto& [bco, nwaveforms] : m_bco_map)
    {
      std::cout << "MicromegasRawDataEvaluation::End - bco: " << bco << ", nwaveforms: " << nwaveforms << std::endl;
    }
  }

  // print bco list, for offline processing
  if (Verbosity())
  {
    std::cout << "const std::vector<uint64_t> lvl1_bco_list = {" << std::endl;
    bool first = true;
    int count = 0;
    for (const auto& [bco, nwaveforms] : m_bco_map)
    {
      if (!first)
      {
        std::cout << ", ";
      }
      first = false;
      if (count == 10)
      {
        count = 0;
        std::cout << std::endl;
      }
      std::cout << " 0x" << std::hex << bco << std::dec;
      ++count;
    }
    std::cout << std::endl
              << "};" << std::endl;
  }

  return Fun4AllReturnCodes::EVENT_OK;
}
