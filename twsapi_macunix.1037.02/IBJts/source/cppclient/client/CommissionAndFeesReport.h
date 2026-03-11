/* Copyright (C) 2024 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

#pragma once
#ifndef TWS_API_CLIENT_COMMISSIONANDFEESREPORT_H
#define TWS_API_CLIENT_COMMISSIONANDFEEDREPORT_H

#include <string>

struct CommissionAndFeesReport
{
	CommissionAndFeesReport()
	{
		commissionAndFees = 0;
		realizedPNL = 0;
		yield = 0;
		yieldRedemptionDate = 0;
	}

	// commissionAndFees report fields
	std::string	execId;
	double		commissionAndFees;
	std::string	currency;
	double		realizedPNL;
	double		yield;
	int			yieldRedemptionDate; // YYYYMMDD format
};

#endif // commissionandfeesreport_def
