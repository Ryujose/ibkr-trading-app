/* Copyright (C) 2025 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */
#pragma once
#ifndef TWS_API_CLIENT_UTILS_H
#define TWS_API_CLIENT_UTILS_H

#include <string>
#include <ctime>
#include "CommonDefs.h"
#include "Decimal.h"

class Utils {

public:
    static bool stringIsEmpty(std::string str);
    static bool isValidValue(int value);
    static bool isValidValue(long value);
    static bool isValidValue(long long value);
    static bool isValidValue(double value);
    static bool isValidValue(Decimal value);
    static bool isPegBenchOrder(std::string orderType);
    static bool isPegMidOrder(std::string orderType);
    static bool isPegBestOrder(std::string orderType);
    static FundDistributionPolicyIndicator getFundDistributionPolicyIndicator(std::string value);
    static FundAssetType getFundAssetType(std::string value);
    static time_t currentTimeMillis();
    static OptionExerciseType getOptionExerciseType(int val);
};

#endif

