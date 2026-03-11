/* Copyright (C) 2024 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

#pragma once

#ifndef TWS_API_CLIENT_INELIGIBILITY_REASON_H
#define TWS_API_CLIENT_INELIGIBILITY_REASON_H

#include <string>

struct IneligibilityReason
{
    IneligibilityReason() {}
    IneligibilityReason(const std::string& p_id, const std::string& p_description)
        : id(p_id), description(p_description)
    {}

    std::string id;
    std::string description;

};

typedef std::shared_ptr<IneligibilityReason> IneligibilityReasonSPtr;
typedef std::vector<IneligibilityReasonSPtr> IneligibilityReasonList;
typedef std::shared_ptr<IneligibilityReasonList> IneligibilityReasonListSPtr;

#endif