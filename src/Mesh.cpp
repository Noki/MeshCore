#include "Mesh.h"
//#include <Arduino.h>

namespace mesh {

void Mesh::begin() {
  Dispatcher::begin();
}

void Mesh::loop() {
  Dispatcher::loop();
}

bool Mesh::allowPacketForward(mesh::Packet* packet) { 
  return false;  // by default, Transport NOT enabled
}

int Mesh::searchPeersByHash(const uint8_t* hash) {
  return 0;  // not found
}

int Mesh::searchChannelsByHash(const uint8_t* hash, GroupChannel channels[], int max_matches) {
  return 0;  // not found
}

DispatcherAction Mesh::onRecvPacket(Packet* pkt) {
  if (pkt->getPayloadVer() > PAYLOAD_VER_1) {  // not supported in this firmware version
    MESH_DEBUG_PRINTLN("Mesh::onRecvPacket(): unsupported packet version");
    return ACTION_RELEASE;
  }

  if (pkt->isRouteDirect() && pkt->path_len >= PATH_HASH_SIZE) {
    if (self_id.isHashMatch(pkt->path) && allowPacketForward(pkt)) {
      // remove our hash from 'path', then re-broadcast
      pkt->path_len -= PATH_HASH_SIZE;
      memcpy(pkt->path, &pkt->path[PATH_HASH_SIZE], pkt->path_len);
      return ACTION_RETRANSMIT(0);   // Routed traffic is HIGHEST priority
    }
    return ACTION_RELEASE;   // this node is NOT the next hop (OR this packet has already been forwarded), so discard.
  }

  DispatcherAction action = ACTION_RELEASE;

  switch (pkt->getPayloadType()) {
    case PAYLOAD_TYPE_ACK: {
      int i = 0;
      uint32_t ack_crc;
      memcpy(&ack_crc, &pkt->payload[i], 4); i += 4;
      if (i > pkt->payload_len) {
        MESH_DEBUG_PRINTLN("Mesh::onRecvPacket(): incomplete ACK packet");
      } else {
        onAckRecv(pkt, ack_crc);
        action = routeRecvPacket(pkt);
      }
      break;
    }
    case PAYLOAD_TYPE_PATH:
    case PAYLOAD_TYPE_REQ:
    case PAYLOAD_TYPE_RESPONSE:
    case PAYLOAD_TYPE_TXT_MSG: {
      int i = 0;
      uint8_t dest_hash = pkt->payload[i++];
      uint8_t src_hash = pkt->payload[i++];

      uint8_t* macAndData = &pkt->payload[i];   // MAC + encrypted data 
      if (i + 2 >= pkt->payload_len) {
        MESH_DEBUG_PRINTLN("Mesh::onRecvPacket(): incomplete data packet");
      } else {
        if (self_id.isHashMatch(&dest_hash)) {
          // scan contacts DB, for all matching hashes of 'src_hash' (max 4 matches supported ATM)
          int num = searchPeersByHash(&src_hash);
          // for each matching contact, try to decrypt data
          bool found = false;
          for (int j = 0; j < num; j++) {
            uint8_t secret[PUB_KEY_SIZE];
            getPeerSharedSecret(secret, j);

            // decrypt, checking MAC is valid
            uint8_t data[MAX_PACKET_PAYLOAD];
            int len = Utils::MACThenDecrypt(secret, data, macAndData, pkt->payload_len - i);
            if (len > 0) {  // success!
              if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH) {
                int k = 0;
                uint8_t path_len = data[k++];
                uint8_t* path = &data[k]; k += path_len;
                uint8_t extra_type = data[k++];
                uint8_t* extra = &data[k];
                uint8_t extra_len = len - k;   // remainder of packet (may be padded with zeroes!)
                onPeerPathRecv(pkt, j, path, path_len, extra_type, extra, extra_len);
              } else {
                onPeerDataRecv(pkt, pkt->getPayloadType(), j, data, len);
              }
              found = true;
              break;
            }
          }
          if (!found) {
            MESH_DEBUG_PRINTLN("recv matches no peers, src_hash=%02X", (uint32_t)src_hash);
          }
        }
        action = routeRecvPacket(pkt);
      }
      break;
    }
    case PAYLOAD_TYPE_ANON_REQ: {
      int i = 0;
      uint8_t dest_hash = pkt->payload[i++];
      uint8_t* sender_pub_key = &pkt->payload[i]; i += PUB_KEY_SIZE;

      uint8_t* macAndData = &pkt->payload[i];   // MAC + encrypted data 
      if (i + 2 >= pkt->payload_len) {
        MESH_DEBUG_PRINTLN("Mesh::onRecvPacket(): incomplete data packet");
      } else {
        if (self_id.isHashMatch(&dest_hash)) {
          Identity sender(sender_pub_key);

          uint8_t secret[PUB_KEY_SIZE];
          self_id.calcSharedSecret(secret, sender);

          // decrypt, checking MAC is valid
          uint8_t data[MAX_PACKET_PAYLOAD];
          int len = Utils::MACThenDecrypt(secret, data, macAndData, pkt->payload_len - i);
          if (len > 0) {  // success!
            onAnonDataRecv(pkt, pkt->getPayloadType(), sender, data, len);
          }
        }
        action = routeRecvPacket(pkt);
      }
      break;
    }
    case PAYLOAD_TYPE_GRP_DATA: 
    case PAYLOAD_TYPE_GRP_TXT: {
      int i = 0;
      uint8_t channel_hash = pkt->payload[i++];

      uint8_t* macAndData = &pkt->payload[i];   // MAC + encrypted data 
      if (i + 2 >= pkt->payload_len) {
        MESH_DEBUG_PRINTLN("Mesh::onRecvPacket(): incomplete data packet");
      } else {
        // scan channels DB, for all matching hashes of 'channel_hash' (max 2 matches supported ATM)
        GroupChannel channels[2];
        int num = searchChannelsByHash(&channel_hash, channels, 2);
        // for each matching channel, try to decrypt data
        for (int j = 0; j < num; j++) {
          // decrypt, checking MAC is valid
          uint8_t data[MAX_PACKET_PAYLOAD];
          int len = Utils::MACThenDecrypt(channels[j].secret, data, macAndData, pkt->payload_len - i);
          if (len > 0) {  // success!
            onGroupDataRecv(pkt, pkt->getPayloadType(), channels[j], data, len);
            break;
          }
        }
        action = routeRecvPacket(pkt);
      }
      break;
    }
    case PAYLOAD_TYPE_ADVERT: {
      int i = 0;
      Identity id;
      memcpy(id.pub_key, &pkt->payload[i], PUB_KEY_SIZE); i += PUB_KEY_SIZE;

      uint32_t timestamp;
      memcpy(&timestamp, &pkt->payload[i], 4); i += 4;
      uint8_t* signature = &pkt->payload[i]; i += SIGNATURE_SIZE;

      if (i > pkt->payload_len) {
        MESH_DEBUG_PRINTLN("Mesh::onRecvPacket(): incomplete advertisement packet");
      } else {
        uint8_t* app_data = &pkt->payload[i];
        int app_data_len = pkt->payload_len - i;
        if (app_data_len > MAX_ADVERT_DATA_SIZE) { app_data_len = MAX_ADVERT_DATA_SIZE; }

        // check that signature is valid
        bool is_ok;
        {
          uint8_t message[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE];
          int msg_len = 0;
          memcpy(&message[msg_len], id.pub_key, PUB_KEY_SIZE); msg_len += PUB_KEY_SIZE;
          memcpy(&message[msg_len], &timestamp, 4); msg_len += 4;
          memcpy(&message[msg_len], app_data, app_data_len); msg_len += app_data_len;

          is_ok = id.verify(signature, message, msg_len);
        }
        if (is_ok) {
          MESH_DEBUG_PRINTLN("Mesh::onRecvPacket(): valid advertisement received!");
          onAdvertRecv(pkt, id, timestamp, app_data, app_data_len);
          action = routeRecvPacket(pkt);
        } else {
          MESH_DEBUG_PRINTLN("Mesh::onRecvPacket(): received advertisement with forged signature!");
        }
      }
      break;
    }
    default:
      MESH_DEBUG_PRINTLN("Mesh::onRecvPacket(): unknown payload type, header: %d", (int) pkt->header);
      action = routeRecvPacket(pkt);
      break;
  }
  return action;
}

DispatcherAction Mesh::routeRecvPacket(Packet* packet) {
  if (packet->isRouteFlood() && packet->path_len + PATH_HASH_SIZE <= MAX_PATH_SIZE && allowPacketForward(packet)) {
    // append this node's hash to 'path'
    packet->path_len += self_id.copyHashTo(&packet->path[packet->path_len]);

    // as this propagates outwards, give it lower and lower priority
    return ACTION_RETRANSMIT(packet->path_len);   // give priority to closer sources, than ones further away
  }
  return ACTION_RELEASE;
}

Packet* Mesh::createAdvert(const LocalIdentity& id, const uint8_t* app_data, size_t app_data_len) {
  if (app_data_len > MAX_ADVERT_DATA_SIZE) return NULL;

  Packet* packet = obtainNewPacket();
  if (packet == NULL) {
    MESH_DEBUG_PRINTLN("Mesh::createAdvert(): error, packet pool empty");
    return NULL;
  }

  packet->header = (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT);  // ROUTE_TYPE_* is set later

  int len = 0;
  memcpy(&packet->payload[len], id.pub_key, PUB_KEY_SIZE); len += PUB_KEY_SIZE;

  uint32_t emitted_timestamp = _rtc->getCurrentTime();
  memcpy(&packet->payload[len], &emitted_timestamp, 4); len += 4;

  uint8_t* signature = &packet->payload[len]; len += SIGNATURE_SIZE;  // will fill this in later

  memcpy(&packet->payload[len], app_data, app_data_len); len += app_data_len;

  packet->payload_len = len;

  {
    uint8_t message[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE];
    int msg_len = 0;
    memcpy(&message[msg_len], id.pub_key, PUB_KEY_SIZE); msg_len += PUB_KEY_SIZE;
    memcpy(&message[msg_len], &emitted_timestamp, 4); msg_len += 4;
    memcpy(&message[msg_len], app_data, app_data_len); msg_len += app_data_len;

    id.sign(signature, message, msg_len);
  }

  return packet;
}

#define MAX_COMBINED_PATH  (MAX_PACKET_PAYLOAD - 2 - CIPHER_BLOCK_SIZE)

Packet* Mesh::createPathReturn(const Identity& dest, const uint8_t* secret, const uint8_t* path, uint8_t path_len, uint8_t extra_type, const uint8_t*extra, size_t extra_len) {
  if (path_len + extra_len + 5 > MAX_COMBINED_PATH) return NULL;  // too long!!

  Packet* packet = obtainNewPacket();
  if (packet == NULL) {
    MESH_DEBUG_PRINTLN("Mesh::createPathReturn(): error, packet pool empty");
    return NULL;
  }
  packet->header = (PAYLOAD_TYPE_PATH << PH_TYPE_SHIFT);  // ROUTE_TYPE_* set later

  int len = 0;
  len += dest.copyHashTo(&packet->payload[len]);  // dest hash
  len += self_id.copyHashTo(&packet->payload[len]);  // src hash

  {
    int data_len = 0;
    uint8_t data[MAX_PACKET_PAYLOAD];

    data[data_len++] = path_len;
    memcpy(&data[data_len], path, path_len); data_len += path_len;
    if (extra_len > 0) {
      data[data_len++] = extra_type;
      memcpy(&data[data_len], extra, extra_len); data_len += extra_len;
    } else {
      // append a timestamp, or random blob (to make packet_hash unique)
      data[data_len++] = 0xFF;  // dummy payload type
      getRNG()->random(&data[data_len], 4); data_len += 4;
    }

    len += Utils::encryptThenMAC(secret, &packet->payload[len], data, data_len);
  }

  packet->payload_len = len;

  return packet;
}

Packet* Mesh::createDatagram(uint8_t type, const Identity& dest, const uint8_t* secret, const uint8_t* data, size_t data_len) {
  if (type == PAYLOAD_TYPE_TXT_MSG || type == PAYLOAD_TYPE_REQ || type == PAYLOAD_TYPE_RESPONSE) {
    if (data_len + 2 + CIPHER_BLOCK_SIZE-1 > MAX_PACKET_PAYLOAD) return NULL;
  } else {
    return NULL;  // invalid type
  }

  Packet* packet = obtainNewPacket();
  if (packet == NULL) {
    MESH_DEBUG_PRINTLN("Mesh::createDatagram(): error, packet pool empty");
    return NULL;
  }
  packet->header = (type << PH_TYPE_SHIFT);  // ROUTE_TYPE_* set later

  int len = 0;
  len += dest.copyHashTo(&packet->payload[len]);  // dest hash
  len += self_id.copyHashTo(&packet->payload[len]);  // src hash
  len += Utils::encryptThenMAC(secret, &packet->payload[len], data, data_len);

  packet->payload_len = len;

  return packet;
}

Packet* Mesh::createAnonDatagram(uint8_t type, const LocalIdentity& sender, const Identity& dest, const uint8_t* secret, const uint8_t* data, size_t data_len) {
  if (type == PAYLOAD_TYPE_ANON_REQ) {
    if (data_len + 1 + PUB_KEY_SIZE + CIPHER_BLOCK_SIZE-1 > MAX_PACKET_PAYLOAD) return NULL;
  } else {
    return NULL;  // invalid type
  }

  Packet* packet = obtainNewPacket();
  if (packet == NULL) {
    MESH_DEBUG_PRINTLN("Mesh::createAnonDatagram(): error, packet pool empty");
    return NULL;
  }
  packet->header = (type << PH_TYPE_SHIFT);  // ROUTE_TYPE_* set later

  int len = 0;
  if (type == PAYLOAD_TYPE_ANON_REQ) {
    len += dest.copyHashTo(&packet->payload[len]);  // dest hash
    memcpy(&packet->payload[len], sender.pub_key, PUB_KEY_SIZE); len += PUB_KEY_SIZE;  // sender pub_key
  } else {
    // FUTURE:
  }
  len += Utils::encryptThenMAC(secret, &packet->payload[len], data, data_len);

  packet->payload_len = len;

  return packet;
}

Packet* Mesh::createGroupDatagram(uint8_t type, const GroupChannel& channel, const uint8_t* data, size_t data_len) {
  if (!(type == PAYLOAD_TYPE_GRP_TXT || type == PAYLOAD_TYPE_GRP_DATA)) return NULL;   // invalid type
  if (data_len + 1 + CIPHER_BLOCK_SIZE-1 > MAX_PACKET_PAYLOAD) return NULL; // too long

  Packet* packet = obtainNewPacket();
  if (packet == NULL) {
    MESH_DEBUG_PRINTLN("Mesh::createGroupDatagram(): error, packet pool empty");
    return NULL;
  }
  packet->header = (type << PH_TYPE_SHIFT);  // ROUTE_TYPE_* set later

  int len = 0;
  memcpy(&packet->payload[len], channel.hash, PATH_HASH_SIZE); len += PATH_HASH_SIZE;
  len += Utils::encryptThenMAC(channel.secret, &packet->payload[len], data, data_len);

  packet->payload_len = len;

  return packet;
}

Packet* Mesh::createAck(uint32_t ack_crc) {
  Packet* packet = obtainNewPacket();
  if (packet == NULL) {
    MESH_DEBUG_PRINTLN("Mesh::createAck(): error, packet pool empty");
    return NULL;
  }
  packet->header = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);  // ROUTE_TYPE_* set later

  memcpy(packet->payload, &ack_crc, 4);
  packet->payload_len = 4;

  return packet;
}

void Mesh::sendFlood(Packet* packet, uint32_t delay_millis) {
  packet->header &= ~PH_ROUTE_MASK;
  packet->header |= ROUTE_TYPE_FLOOD;
  packet->path_len = 0;

  allowPacketForward(packet);  // mark this packet as already sent in case it is rebroadcast back to us

  uint8_t pri;
  if (packet->getPayloadType() == PAYLOAD_TYPE_PATH) {
    pri = 2;
  } else if (packet->getPayloadType() == PAYLOAD_TYPE_ADVERT) {
    pri = 3;   // de-prioritie these
  } else {
    pri = 1;
  }
  sendPacket(packet, pri, delay_millis);
}

void Mesh::sendDirect(Packet* packet, const uint8_t* path, uint8_t path_len, uint32_t delay_millis) {
  packet->header &= ~PH_ROUTE_MASK;
  packet->header |= ROUTE_TYPE_DIRECT;

  memcpy(packet->path, path, packet->path_len = path_len);

  allowPacketForward(packet);  // mark this packet as already sent in case it is rebroadcast back to us

  sendPacket(packet, 0, delay_millis);
}

void Mesh::sendZeroHop(Packet* packet, uint32_t delay_millis) {
  packet->header &= ~PH_ROUTE_MASK;
  packet->header |= ROUTE_TYPE_DIRECT;

  packet->path_len = 0;  // path_len of zero means Zero Hop

  allowPacketForward(packet);  // mark this packet as already sent in case it is rebroadcast back to us

  sendPacket(packet, 0, delay_millis);
}

}