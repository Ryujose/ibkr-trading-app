/* Copyright (C) 2025 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

#include "StdAfx.h"
#include "Utils.h"

bool Utils::stringIsEmpty(std::string str) {
    return str.empty();
}

bool Utils::isValidValue(int value) {
    return value != UNSET_INTEGER;
}

bool Utils::isValidValue(long value) {
    return value != UNSET_LONG;
}

bool Utils::isValidValue(long long value) {
    return value != UNSET_LLONG;
}

bool Utils::isValidValue(double value) {
    return value != UNSET_DOUBLE;
}

bool Utils::isValidValue(Decimal value) {
    return value != UNSET_DECIMAL;
}

bool Utils::isPegBenchOrder(std::string orderType) {
    return orderType == "PEG BENCH" || orderType == "PEGBENCH";
}

bool Utils::isPegMidOrder(std::string orderType) {
    return orderType == "PEG MID" || orderType == "PEGMID";
}

bool Utils::isPegBestOrder(std::string orderType) {
    return orderType == "PEG BEST" || orderType == "PEGBEST";
}

FundAssetType Utils::getFundAssetType(std::string value) {
    if (value == "000") {
        return FundAssetType::Others;
    }
    else if (value == "001") {
        return FundAssetType::MoneyMarket;
    }
    else if (value == "002") {
        return FundAssetType::FixedIncome;
    }
    else if (value == "003") {
        return FundAssetType::MultiAsset;
    }
    else if (value == "004") {
        return FundAssetType::Equity;
    }
    else if (value == "005") {
        return FundAssetType::Sector;
    }
    else if (value == "006") {
        return FundAssetType::Guaranteed;
    }
    else if (value == "007") {
        return FundAssetType::Alternative;
    }
    return FundAssetType::None;
}

FundDistributionPolicyIndicator Utils::getFundDistributionPolicyIndicator(std::string value) {
    if (value == "N") {
        return FundDistributionPolicyIndicator::AccumulationFund;
    }
    else if (value == "Y") {
        return FundDistributionPolicyIndicator::IncomeFund;
    }
    return FundDistributionPolicyIndicator::None;
}

time_t Utils::currentTimeMillis() {
    return time(NULL) * 1000;
}

OptionExerciseType Utils::getOptionExerciseType(int val) {
    if (val == -1) {
        return OptionExerciseType::None;
    }
    else if (val == 1) {
        return OptionExerciseType::Exercise;
    }
    else if (val == 2) {
        return OptionExerciseType::Lapse;
    }
    else if (val == 3) {
        return OptionExerciseType::DoNothing;
    }
    else if (val == 100) {
        return OptionExerciseType::Assigned;
    }
    else if (val == 101) {
        return OptionExerciseType::AutoexerciseClearing;
    }
    else if (val == 102) {
        return OptionExerciseType::Expired;
    }
    else if (val == 103) {
        return OptionExerciseType::Netting;
    }
    else if (val == 200) {
        return OptionExerciseType::AutoexerciseTrading;
    }
    return OptionExerciseType::None;
}
