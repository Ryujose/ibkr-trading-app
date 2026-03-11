/* Copyright (C) 2024 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

#pragma once
#ifndef TWS_API_CLIENT_ORDERSTATE_H
#define TWS_API_CLIENT_ORDERSTATE_H

#include "Order.h"

struct OrderAllocation
{
	OrderAllocation()
	{
		account = "";
		position = UNSET_DECIMAL;
		positionDesired = UNSET_DECIMAL;
		positionAfter = UNSET_DECIMAL;
		desiredAllocQty = UNSET_DECIMAL;
		allowedAllocQty = UNSET_DECIMAL;
		isMonetary = false;
	}

	std::string account;
	Decimal position;
	Decimal positionDesired;
	Decimal positionAfter;
	Decimal desiredAllocQty;
	Decimal allowedAllocQty;
	bool isMonetary;

	bool operator==(const OrderAllocation& other) const
	{
		return (account == other.account);
	}
};

typedef std::shared_ptr<OrderAllocation> OrderAllocationSPtr;
typedef std::vector<OrderAllocationSPtr> OrderAllocationList;
typedef std::shared_ptr<OrderAllocationList> OrderAllocationListSPtr;

struct OrderState {

	explicit OrderState()
		:
		commissionAndFees(UNSET_DOUBLE),
		minCommissionAndFees(UNSET_DOUBLE),
		maxCommissionAndFees(UNSET_DOUBLE),
		initMarginBeforeOutsideRTH(UNSET_DOUBLE),
		maintMarginBeforeOutsideRTH(UNSET_DOUBLE),
		equityWithLoanBeforeOutsideRTH(UNSET_DOUBLE),
		initMarginChangeOutsideRTH(UNSET_DOUBLE),
		maintMarginChangeOutsideRTH(UNSET_DOUBLE),
		equityWithLoanChangeOutsideRTH(UNSET_DOUBLE),
		initMarginAfterOutsideRTH(UNSET_DOUBLE),
		maintMarginAfterOutsideRTH(UNSET_DOUBLE),
		equityWithLoanAfterOutsideRTH(UNSET_DOUBLE),
		suggestedSize(UNSET_DECIMAL)
	{}

	std::string status;

	std::string initMarginBefore;
	std::string maintMarginBefore;
	std::string equityWithLoanBefore;
	std::string initMarginChange;
	std::string maintMarginChange;
	std::string equityWithLoanChange;
	std::string initMarginAfter;
	std::string maintMarginAfter;
	std::string equityWithLoanAfter;

	double  commissionAndFees;
	double  minCommissionAndFees;
	double  maxCommissionAndFees;
	std::string commissionAndFeesCurrency;
	std::string marginCurrency;
	double initMarginBeforeOutsideRTH;
	double maintMarginBeforeOutsideRTH;
	double equityWithLoanBeforeOutsideRTH;
	double initMarginChangeOutsideRTH;
	double maintMarginChangeOutsideRTH;
	double equityWithLoanChangeOutsideRTH;
	double initMarginAfterOutsideRTH;
	double maintMarginAfterOutsideRTH;
	double equityWithLoanAfterOutsideRTH;
	Decimal suggestedSize;
	std::string rejectReason;
	OrderAllocationListSPtr orderAllocations;
	std::string warningText;

	std::string completedTime;
	std::string completedStatus;
};

#endif
