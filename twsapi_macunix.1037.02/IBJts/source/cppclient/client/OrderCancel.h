/* Copyright (C) 2025 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

#pragma once
#ifndef TWS_API_CLIENT_ORDER_CANCEL_H
#define TWS_API_CLIENT_ORDER_CANCEL_H

#include <string>

struct OrderCancel
{
    std::string manualOrderCancelTime = "";
    std::string extOperator = "";
    int manualOrderIndicator = UNSET_INTEGER;
};

#endif
