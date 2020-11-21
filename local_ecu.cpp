/**
 *
 * Author : Author Daniel Movsesyan
 * Created On : 11/16/2020 15:0:44
 * File : local_ecu.cpp
 *
 */
    
#include "local_ecu.hpp"  
#include "can_processor.hpp"  

#include <mutex>

namespace brt {
namespace can {

/**
 * \fn  constructor LocalECU::LocalECU
 *
 * @param  processor : CanProcessor* 
 * @param  name : const CanName& 
 */
LocalECU::LocalECU(CanProcessor* processor,const CanName& name /*= CanName()*/)
: CanECU(processor,name)
, _mutex(processor)
{

}

/**
 * \fn  destructor LocalECU::~LocalECU
 *
 */
LocalECU::~LocalECU()
{

}

/**
 * \fn  LocalECU::claim_address
 *
 * @param  address : uint8_t 
 * @param  & bus : const std::string
 */
void LocalECU::claim_address(uint8_t address,const std::string& bus_name)
{
  if (address == NULL_CAN_ADDRESS)
  {
    // Claiming NULL address means we are sending Cannot Claim Address message,
    // which automatically deactivates the device
    disable_device(bus_name);
  }
  else
  {
    std::lock_guard<Mutex> l(_mutex);

    auto container = _container_map.find(bus_name);
    if (container == _container_map.end())
    {
      auto pair = _container_map.insert(ContainerMap::value_type(bus_name, Container()));
      if (!pair.second)
        return; // Assert
      
      container = pair.first;
    }
    // With Normal address claimed sent we should put the device into waiting state for
    // 250 ms according to ISO 11783-5 requirements
    container->second._status = eWaiting;
    container->second._time_tag = processor()->get_time_tick();
    
    processor()->register_updater([this, bus_name]()->bool
    {
      uint64_t cur_time = processor()->get_time_tick();
      std::vector<std::pair<CanMessagePtr,RemoteECUPtr>> array;
      
      {
        std::lock_guard<Mutex> l(_mutex);
        auto container = _container_map.find(bus_name);

        if (container->second._status != eWaiting)
          return true; // remove from updaters
       
        if ((cur_time - container->second._time_tag) < CAN_ADDRESS_CLAIMED_WAITING_TIME)
          return false;

        container->second._status = eActive;
        while (!container->second._fifo.empty())
        {
          Queue& queue = container->second._fifo.front();
          array.push_back(std::pair<CanMessagePtr,RemoteECUPtr>(queue._message, queue._remote));
          container->second._fifo.pop_front();
        }
      }

      for (auto pair : array)
        send_message(pair.first, pair.second);

      return true;
    });
  }

  processor()->send_raw_packet(CanPacket(name().data(), sizeof(uint64_t), PGN_AddressClaimed, BROADCATS_CAN_ADDRESS, address),bus_name);
}

/**
 * \fn  LocalECU::send_message
 *
 * @param  message : CanMessagePtr 
 * @param  remote : RemoteECUPtr 
 */
bool LocalECU::send_message(CanMessagePtr message,RemoteECUPtr remote)
{
  std::unordered_map<std::string,uint8_t> sa_map = get_addresses();
  std::vector<std::pair<std::string,uint8_t>> bus_address;
  bool pushed_to_fifo = false;
  {
    std::lock_guard<Mutex> l(_mutex);
    if (!remote)
    {
      // broadcast
      for (auto container : _container_map)
      {
        auto sa = sa_map.find(container.first);
        if (sa == sa_map.end())
          continue;

        if (container.second._status == eInavtive)
          continue;

        if (container.second._status == eWaiting)
        {
          container.second._fifo.push_back(Queue(message, remote));
          continue;
        }
        bus_address.push_back(std::pair<std::string,uint8_t>(container.first, BROADCATS_CAN_ADDRESS));
      }
    }
    else
    {
      std::vector<std::string> buses = processor()->device_db().get_ecu_bus(remote->name());
      for (auto bus_name : buses)
      {
        auto container = _container_map.find(bus_name);
        if (container == _container_map.end())
          continue;

        auto sa = sa_map.find(buses[0]);
        if (sa == sa_map.end())
          continue;

        if (container->second._status == eInavtive)
          continue;

        if (container->second._status == eWaiting)
        {
          container->second._fifo.push_back(Queue(message, remote));
          pushed_to_fifo = true;
          continue;
        }

        bus_address.push_back(std::pair<std::string,uint8_t>(bus_name, sa->second));
      }
    }
  }

  if (bus_address.empty() && !pushed_to_fifo)
    return false;

  for (auto ba : bus_address)
  {
    if (message->length() <= 8)
    {
      if (!processor()->send_raw_packet(CanPacket(message->data(),message->pgn(),
                    remote->get_address(),ba.second,message->priority()), ba.first))
        return false;
    }
  }

  return true;
}

/**
 * \fn  LocalECU::disable_device
 *
 * @param  & bus : const std::string
 */
void LocalECU::disable_device(const std::string& bus)
{
  std::lock_guard<Mutex> l(_mutex);
  auto stat = _container_map.find(bus);
  if (stat != _container_map.end())
    stat->second._status = eInavtive;
}

} // can
} // brt
