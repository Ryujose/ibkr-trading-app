/* Copyright (C) 2025 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

#pragma once
#ifndef TWS_API_CLIENT_EDECODER_UTILS_H
#define TWS_API_CLIENT_EDECODER_UTILS_H

#include "Contract.h"
#include "Execution.h"
#include "Order.h"
#include "OrderState.h"
#include "OperatorCondition.h"
#include "ContractCondition.h"
#include "PriceCondition.h"
#include "TimeCondition.h"
#include "MarginCondition.h"
#include "ExecutionCondition.h"
#include "VolumeCondition.h"
#include "PercentChangeCondition.h"

#include "ExecutionDetails.pb.h"
#include "Order.pb.h"
#include "OrderState.pb.h"

class EDecoderUtils {

public:
    static Contract decodeContract(const protobuf::Contract& contractProto);
    static Contract::ComboLegListSPtr decodeComboLegs(const protobuf::Contract& contractProto);
    static Order::OrderComboLegListSPtr decodeOrderComboLegs(const protobuf::Contract& contractProto);
    static DeltaNeutralContract* decodeDeltaNeutralContract(const protobuf::Contract& contractProto);
    static Execution decodeExecution(const protobuf::Execution& executionProto);
    static Order decodeOrder(const protobuf::Contract& contractProto, const protobuf::Order& orderProto);
    static std::vector<std::shared_ptr<OrderCondition>> decodeConditions(const protobuf::Order& order);
    static void setConditionFields(const protobuf::OrderCondition& orderConditionProto, OrderCondition& orderCondition);
    static void setOperatorConditionFields(const protobuf::OrderCondition& orderConditionProto, OperatorCondition& operatorCondition);
    static void setContractConditionFields(const protobuf::OrderCondition& orderConditionProto, ContractCondition& contractCondition);
    static PriceCondition* createPriceCondition(const protobuf::OrderCondition& orderConditionProto);
    static TimeCondition* createTimeCondition(const protobuf::OrderCondition& orderConditionProto);
    static MarginCondition* createMarginCondition(const protobuf::OrderCondition& orderConditionProto);
    static ExecutionCondition* createExecutionCondition(const protobuf::OrderCondition& orderConditionProto);
    static VolumeCondition* createVolumeCondition(const protobuf::OrderCondition& orderConditionProto);
    static PercentChangeCondition* createPercentChangeCondition(const protobuf::OrderCondition& orderConditionProto);
    static SoftDollarTier decodeSoftDollarTier(const protobuf::Order& order);
    static TagValueListSPtr decodeTagValueList(google::protobuf::Map<std::string, std::string> stringStringMap);
    static OrderState decodeOrderState(const protobuf::OrderState& orderStateProto);
    static OrderAllocationListSPtr decodeOrderAllocations(const protobuf::OrderState& orderStateProto);
};

#endif

