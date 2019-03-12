//
//  LoxBusTreeExtension.cpp
//
//  Created by Markus Fritze on 06.03.19.
//  Copyright (c) 2019 Markus Fritze. All rights reserved.
//

#include "LoxBusTreeExtension.hpp"
#include "global_functions.hpp"
#include <stdio.h>

LoxBusTreeExtension::LoxBusTreeExtension(LoxCANBaseDriver &driver, uint32_t serial, eAliveReason_t alive)
  : LoxNATExtension(driver, (serial & 0xFFFFFF) | (eDeviceType_t_TreeBaseExtension << 24), eDeviceType_t_TreeBaseExtension, 0, 9030305, 0, sizeof(config), &config, alive), treeDevicesLeftCount(0), treeDevicesRightCount(0) {
}

/***
 *  Adding a device to the Tree Bus Extension
 ***/
void LoxBusTreeExtension::AddExtension(LoxBusTreeDevice *ext, eTreeBranch branch) {
  if (branch == eTreeBranch_leftBranch) {
    assert(this->treeDevicesLeftCount == MAX_TREE_DEVICECOUNT);
    this->treeDevicesLeft[this->treeDevicesLeftCount++] = ext;
  } else if (branch == eTreeBranch_rightBranch) {
    assert(this->treeDevicesRightCount == MAX_TREE_DEVICECOUNT);
    this->treeDevicesRight[this->treeDevicesRightCount++] = ext;
  }
}

/***
 *  Send values after the start
 ***/
void LoxBusTreeExtension::SendValues(void) {
  send_can_status(CAN_Error_Reply, eTreeBranch_leftBranch);
  send_can_status(CAN_Error_Reply, eTreeBranch_rightBranch);
}

/***
 *  A direct message received
 ***/
void LoxBusTreeExtension::ReceiveDirect(LoxCanMessage &message) {
  if (message.deviceNAT != 0) { // forward to a tree device?
    message.busType = LoxCmdNATBus_t_TreeBus;
    uint8_t nat = message.deviceNAT;
    message.extensionNat = nat;
    // messages to parked devices is sent to both branches for parked devices, except for a NAT offer
    if ((nat & 0x80) == 0x80 and message.commandNat == NAT_Offer) {
      for (int i = 0; i < this->treeDevicesLeftCount; ++i)
        this->treeDevicesLeft[i]->ReceiveMessage(message);
      for (int i = 0; i < this->treeDevicesRightCount; ++i)
        this->treeDevicesRight[i]->ReceiveMessage(message);
    } else {
      if (message.commandNat == NAT_Offer)
        nat = message.data[1]; // for NAT offset use the new NAT
      if (nat & 0x40) {        // left branch?
        for (int i = 0; i < this->treeDevicesLeftCount; ++i)
          this->treeDevicesLeft[i]->ReceiveMessage(message);
      } else { // right branch
        for (int i = 0; i < this->treeDevicesRightCount; ++i)
          this->treeDevicesRight[i]->ReceiveMessage(message);
      }
    }
  } else {
    LoxCanMessage msg;
    switch (message.commandNat) {
    case CAN_Diagnosis_Request:
      if (message.value16 != 0) { // for this device (!=0 => for a Tree bus)
        send_can_status(CAN_Diagnosis_Reply, eTreeBranch(message.value16));
      }
      break;
    case CAN_Error_Request:
      if (message.value16 != 0) { // for this device (!=0 => for a Tree bus)
        send_can_status(CAN_Error_Reply, eTreeBranch(message.value16));
      }
      break;
    default:
      break;
    }
    LoxNATExtension::ReceiveDirect(message);
  }
}

// Forward other messages to the tree devices
void LoxBusTreeExtension::ReceiveBroadcast(LoxCanMessage &message) {
  LoxNATExtension::ReceiveBroadcast(message);

  message.busType = LoxCmdNATBus_t_TreeBus;
  uint8_t nat = message.deviceNAT;
  message.extensionNat = nat;
  // messages to parked devices is sent to both branches for parked devices, except for a NAT offer
  if ((nat & 0x80) == 0x80 and message.commandNat == NAT_Offer) {
    for (int i = 0; i < this->treeDevicesLeftCount; ++i)
      this->treeDevicesLeft[i]->ReceiveBroadcast(message);
    for (int i = 0; i < this->treeDevicesRightCount; ++i)
      this->treeDevicesRight[i]->ReceiveBroadcast(message);
  } else {
    if (message.commandNat == NAT_Offer)
      nat = message.data[1]; // for NAT offset use the new NAT
    if (nat & 0x40) {        // left branch?
      for (int i = 0; i < this->treeDevicesLeftCount; ++i)
        this->treeDevicesLeft[i]->ReceiveBroadcast(message);
    } else { // right branch
      for (int i = 0; i < this->treeDevicesRightCount; ++i)
        this->treeDevicesRight[i]->ReceiveBroadcast(message);
    }
  }
}

void LoxBusTreeExtension::ReceiveDirectFragment(LoxMsgNATCommand_t command, const uint8_t *data, uint16_t size) {
  LoxNATExtension::ReceiveDirectFragment(command, data, size);
}

void LoxBusTreeExtension::ReceiveBroadcastFragment(LoxMsgNATCommand_t command, const uint8_t *data, uint16_t size) {
  LoxNATExtension::ReceiveBroadcastFragment(command, data, size);
  for (int i = 0; i < this->treeDevicesLeftCount; ++i)
    this->treeDevicesLeft[i]->ReceiveBroadcastFragment(command, data, size);
  for (int i = 0; i < this->treeDevicesRightCount; ++i)
    this->treeDevicesRight[i]->ReceiveBroadcastFragment(command, data, size);
}

/***
 *  10ms Timer to be called 100x per second
 ***/
void LoxBusTreeExtension::Timer10ms() {
  for (int i = 0; i < this->treeDevicesLeftCount; ++i)
    this->treeDevicesLeft[i]->Timer10ms();
  for (int i = 0; i < this->treeDevicesRightCount; ++i)
    this->treeDevicesRight[i]->Timer10ms();
}