/* Copyright (C) 2025 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

#include "StdAfx.h"
#include "EClientUtils.h"
#include "Utils.h"
#include "OperatorCondition.h"
#include "ContractCondition.h"
#include "PriceCondition.h"
#include "TimeCondition.h"
#include "MarginCondition.h"
#include "ExecutionCondition.h"
#include "VolumeCondition.h"
#include "PercentChangeCondition.h"

protobuf::ExecutionRequest EClientUtils::createExecutionRequestProto(int reqId, const ExecutionFilter& filter) {
    protobuf::ExecutionRequest executionRequestProto;
    executionRequestProto.set_reqid(reqId);

    protobuf::ExecutionFilter* executionFilterProto = executionRequestProto.mutable_executionfilter();
    if (Utils::isValidValue(filter.m_clientId)) executionFilterProto->set_clientid(filter.m_clientId);
    if (!Utils::stringIsEmpty(filter.m_acctCode)) executionFilterProto->set_acctcode(filter.m_acctCode);
    if (!Utils::stringIsEmpty(filter.m_time)) executionFilterProto->set_time(filter.m_time);
    if (!Utils::stringIsEmpty(filter.m_symbol)) executionFilterProto->set_symbol(filter.m_symbol);
    if (!Utils::stringIsEmpty(filter.m_secType)) executionFilterProto->set_sectype(filter.m_secType);
    if (!Utils::stringIsEmpty(filter.m_exchange)) executionFilterProto->set_exchange(filter.m_exchange);
    if (!Utils::stringIsEmpty(filter.m_side)) executionFilterProto->set_side(filter.m_side);
    if (Utils::isValidValue(filter.m_lastNDays)) executionFilterProto->set_lastndays(filter.m_lastNDays);
    if (!filter.m_specificDates.empty()) {
        for (long specificDate : filter.m_specificDates) {
            executionFilterProto->add_specificdates(specificDate);
        }
    }
    return executionRequestProto;
}

protobuf::PlaceOrderRequest EClientUtils::createPlaceOrderRequestProto(OrderId orderId, const Contract& contract, const Order& order) {
    protobuf::PlaceOrderRequest placeOrderRequestProto;
    if (Utils::isValidValue(orderId)) placeOrderRequestProto.set_orderid(orderId);
    placeOrderRequestProto.mutable_contract()->CopyFrom(createContractProto(contract, order));
    placeOrderRequestProto.mutable_order()->CopyFrom(createOrderProto(order));
    return placeOrderRequestProto;
}

protobuf::Order EClientUtils::createOrderProto(const Order& order) {
    protobuf::Order orderProto;
    if (Utils::isValidValue(order.clientId)) orderProto.set_clientid(order.clientId);
    if (Utils::isValidValue(order.permId)) orderProto.set_permid(order.permId);
    if (Utils::isValidValue(order.parentId)) orderProto.set_parentid(order.parentId);
    if (!Utils::stringIsEmpty(order.action)) orderProto.set_action(order.action.c_str());
    if (Utils::isValidValue(order.totalQuantity)) orderProto.set_totalquantity(DecimalFunctions::decimalStringToDisplay(order.totalQuantity));
    if (Utils::isValidValue(order.displaySize)) orderProto.set_displaysize(order.displaySize);
    if (!Utils::stringIsEmpty(order.orderType)) orderProto.set_ordertype(order.orderType);
    if (Utils::isValidValue(order.lmtPrice)) orderProto.set_lmtprice(order.lmtPrice);
    if (Utils::isValidValue(order.auxPrice)) orderProto.set_auxprice(order.auxPrice);
    if (!Utils::stringIsEmpty(order.tif)) orderProto.set_tif(order.tif);
    if (!Utils::stringIsEmpty(order.account)) orderProto.set_account(order.account);
    if (!Utils::stringIsEmpty(order.settlingFirm)) orderProto.set_settlingfirm(order.settlingFirm);
    if (!Utils::stringIsEmpty(order.clearingAccount)) orderProto.set_clearingaccount(order.clearingAccount);
    if (!Utils::stringIsEmpty(order.clearingIntent)) orderProto.set_clearingintent(order.clearingIntent);
    if (order.allOrNone) orderProto.set_allornone(order.allOrNone);
    if (order.blockOrder) orderProto.set_blockorder(order.blockOrder);
    if (order.hidden) orderProto.set_hidden(order.hidden);
    if (order.outsideRth) orderProto.set_outsiderth(order.outsideRth);
    if (order.sweepToFill) orderProto.set_sweeptofill(order.sweepToFill);
    if (Utils::isValidValue(order.percentOffset)) orderProto.set_percentoffset(order.percentOffset);
    if (Utils::isValidValue(order.trailingPercent)) orderProto.set_trailingpercent(order.trailingPercent);
    if (Utils::isValidValue(order.trailStopPrice)) orderProto.set_trailstopprice(order.trailStopPrice);
    if (Utils::isValidValue(order.minQty)) orderProto.set_minqty(order.minQty);
    if (!Utils::stringIsEmpty(order.goodAfterTime)) orderProto.set_goodaftertime(order.goodAfterTime);
    if (!Utils::stringIsEmpty(order.goodTillDate)) orderProto.set_goodtilldate(order.goodTillDate);
    if (!Utils::stringIsEmpty(order.ocaGroup)) orderProto.set_ocagroup(order.ocaGroup);
    if (!Utils::stringIsEmpty(order.orderRef)) orderProto.set_orderref(order.orderRef);
    if (!Utils::stringIsEmpty(order.rule80A)) orderProto.set_rule80a(order.rule80A);
    if (Utils::isValidValue(order.ocaType)) orderProto.set_ocatype(order.ocaType);
    if (Utils::isValidValue(order.triggerMethod)) orderProto.set_triggermethod(order.triggerMethod);
    if (!Utils::stringIsEmpty(order.activeStartTime)) orderProto.set_activestarttime(order.activeStartTime);
    if (!Utils::stringIsEmpty(order.activeStopTime)) orderProto.set_activestoptime(order.activeStopTime);
    if (!Utils::stringIsEmpty(order.faGroup)) orderProto.set_fagroup(order.faGroup);
    if (!Utils::stringIsEmpty(order.faMethod)) orderProto.set_famethod(order.faMethod);
    if (!Utils::stringIsEmpty(order.faPercentage)) orderProto.set_fapercentage(order.faPercentage);
    if (Utils::isValidValue(order.volatility))  orderProto.set_volatility(order.volatility);
    if (Utils::isValidValue(order.volatilityType)) orderProto.set_volatilitytype(order.volatilityType);
    if (Utils::isValidValue(order.continuousUpdate)) orderProto.set_continuousupdate(order.continuousUpdate);
    if (Utils::isValidValue(order.referencePriceType)) orderProto.set_referencepricetype(order.referencePriceType);
    if (!Utils::stringIsEmpty(order.deltaNeutralOrderType)) orderProto.set_deltaneutralordertype(order.deltaNeutralOrderType);
    if (Utils::isValidValue(order.deltaNeutralAuxPrice)) orderProto.set_deltaneutralauxprice(order.deltaNeutralAuxPrice);
    if (Utils::isValidValue(order.deltaNeutralConId)) orderProto.set_deltaneutralconid(order.deltaNeutralConId);
    if (!Utils::stringIsEmpty(order.deltaNeutralOpenClose)) orderProto.set_deltaneutralopenclose(order.deltaNeutralOpenClose);
    if (order.deltaNeutralShortSale) orderProto.set_deltaneutralshortsale(order.deltaNeutralShortSale);
    if (Utils::isValidValue(order.deltaNeutralShortSaleSlot)) orderProto.set_deltaneutralshortsaleslot(order.deltaNeutralShortSaleSlot);
    if (!Utils::stringIsEmpty(order.deltaNeutralDesignatedLocation)) orderProto.set_deltaneutraldesignatedlocation(order.deltaNeutralDesignatedLocation);
    if (Utils::isValidValue(order.scaleInitLevelSize)) orderProto.set_scaleinitlevelsize(order.scaleInitLevelSize);
    if (Utils::isValidValue(order.scaleSubsLevelSize)) orderProto.set_scalesubslevelsize(order.scaleSubsLevelSize);
    if (Utils::isValidValue(order.scalePriceIncrement)) orderProto.set_scalepriceincrement(order.scalePriceIncrement);
    if (Utils::isValidValue(order.scalePriceAdjustValue)) orderProto.set_scalepriceadjustvalue(order.scalePriceAdjustValue);
    if (Utils::isValidValue(order.scalePriceAdjustInterval)) orderProto.set_scalepriceadjustinterval(order.scalePriceAdjustInterval);
    if (Utils::isValidValue(order.scaleProfitOffset)) orderProto.set_scaleprofitoffset(order.scaleProfitOffset);
    if (order.scaleAutoReset) orderProto.set_scaleautoreset(order.scaleAutoReset);
    if (Utils::isValidValue(order.scaleInitPosition)) orderProto.set_scaleinitposition(order.scaleInitPosition);
    if (Utils::isValidValue(order.scaleInitFillQty)) orderProto.set_scaleinitfillqty(order.scaleInitFillQty);
    if (order.scaleRandomPercent) orderProto.set_scalerandompercent(order.scaleRandomPercent);
    if (!Utils::stringIsEmpty(order.scaleTable)) orderProto.set_scaletable(order.scaleTable);
    if (!Utils::stringIsEmpty(order.hedgeType)) orderProto.set_hedgetype(order.hedgeType);
    if (!Utils::stringIsEmpty(order.hedgeParam)) orderProto.set_hedgeparam(order.hedgeParam);

    if (!Utils::stringIsEmpty(order.algoStrategy)) {
        orderProto.set_algostrategy(order.algoStrategy);

        std::map<std::string, std::string> algoParamsMap = createStringStringMap(order.algoParams);
        for (std::pair<std::string, std::string> algoParam : algoParamsMap) {
            (*orderProto.mutable_algoparams())[algoParam.first] = algoParam.second;
        }
    }
    if (!Utils::stringIsEmpty(order.algoId)) orderProto.set_algoid(order.algoId);

    std::map<std::string, std::string> smartComboRoutingParamsMap = createStringStringMap(order.smartComboRoutingParams);
    for (std::pair<std::string, std::string> smartComboRoutingParam : smartComboRoutingParamsMap) {
        (*orderProto.mutable_smartcomboroutingparams())[smartComboRoutingParam.first] = smartComboRoutingParam.second;
    }

    if (order.whatIf) orderProto.set_whatif(order.whatIf);
    if (order.transmit) orderProto.set_transmit(order.transmit);
    if (order.overridePercentageConstraints) orderProto.set_overridepercentageconstraints(order.overridePercentageConstraints);
    if (!Utils::stringIsEmpty(order.openClose)) orderProto.set_openclose(order.openClose);
    if (Utils::isValidValue(order.origin)) orderProto.set_origin(order.origin);
    if (Utils::isValidValue(order.shortSaleSlot)) orderProto.set_shortsaleslot(order.shortSaleSlot);
    if (!Utils::stringIsEmpty(order.designatedLocation)) orderProto.set_designatedlocation(order.designatedLocation);
    if (Utils::isValidValue(order.exemptCode)) orderProto.set_exemptcode(order.exemptCode);
    if (!Utils::stringIsEmpty(order.deltaNeutralSettlingFirm)) orderProto.set_deltaneutralsettlingfirm(order.deltaNeutralSettlingFirm);
    if (!Utils::stringIsEmpty(order.deltaNeutralClearingAccount)) orderProto.set_deltaneutralclearingaccount(order.deltaNeutralClearingAccount);
    if (!Utils::stringIsEmpty(order.deltaNeutralClearingIntent)) orderProto.set_deltaneutralclearingintent(order.deltaNeutralClearingIntent);
    if (Utils::isValidValue(order.discretionaryAmt)) orderProto.set_discretionaryamt(order.discretionaryAmt);
    if (order.optOutSmartRouting) orderProto.set_optoutsmartrouting(order.optOutSmartRouting);
    if (Utils::isValidValue(order.exemptCode)) orderProto.set_exemptcode(order.exemptCode);
    if (Utils::isValidValue(order.startingPrice)) orderProto.set_startingprice(order.startingPrice);
    if (Utils::isValidValue(order.stockRefPrice)) orderProto.set_stockrefprice(order.stockRefPrice);
    if (Utils::isValidValue(order.delta)) orderProto.set_delta(order.delta);
    if (Utils::isValidValue(order.stockRangeLower)) orderProto.set_stockrangelower(order.stockRangeLower);
    if (Utils::isValidValue(order.stockRangeUpper)) orderProto.set_stockrangeupper(order.stockRangeUpper);
    if (order.notHeld) orderProto.set_notheld(order.notHeld);

    std::map<std::string, std::string> orderMiscOptionsMap = createStringStringMap(order.orderMiscOptions);
    for (std::pair<std::string, std::string> orderMiscOption : orderMiscOptionsMap) {
        (*orderProto.mutable_ordermiscoptions())[orderMiscOption.first] = orderMiscOption.second;
    }

    if (order.solicited) orderProto.set_solicited(order.solicited);
    if (order.randomizeSize) orderProto.set_randomizesize(order.randomizeSize);
    if (order.randomizePrice) orderProto.set_randomizeprice(order.randomizePrice);
    if (Utils::isValidValue(order.referenceContractId)) orderProto.set_referencecontractid(order.referenceContractId);
    if (Utils::isValidValue(order.peggedChangeAmount)) orderProto.set_peggedchangeamount(order.peggedChangeAmount);
    if (order.isPeggedChangeAmountDecrease) orderProto.set_ispeggedchangeamountdecrease(order.isPeggedChangeAmountDecrease);
    if (Utils::isValidValue(order.referenceChangeAmount)) orderProto.set_referencechangeamount(order.referenceChangeAmount);
    if (!Utils::stringIsEmpty(order.referenceExchangeId)) orderProto.set_referenceexchangeid(order.referenceExchangeId);
    if (!Utils::stringIsEmpty(order.adjustedOrderType)) orderProto.set_adjustedordertype(order.adjustedOrderType);
    if (Utils::isValidValue(order.triggerPrice)) orderProto.set_triggerprice(order.triggerPrice);
    if (Utils::isValidValue(order.adjustedStopPrice)) orderProto.set_adjustedstopprice(order.adjustedStopPrice);
    if (Utils::isValidValue(order.adjustedStopLimitPrice)) orderProto.set_adjustedstoplimitprice(order.adjustedStopLimitPrice);
    if (Utils::isValidValue(order.adjustedTrailingAmount)) orderProto.set_adjustedtrailingamount(order.adjustedTrailingAmount);
    if (Utils::isValidValue(order.adjustableTrailingUnit)) orderProto.set_adjustabletrailingunit(order.adjustableTrailingUnit);
    if (Utils::isValidValue(order.lmtPriceOffset)) orderProto.set_lmtpriceoffset(order.lmtPriceOffset);

    std::list<protobuf::OrderCondition> orderConditionList = createConditionsProto(order);
    if (!orderConditionList.empty()) {
        for (protobuf::OrderCondition orderConditionProto : orderConditionList) {
            orderProto.add_conditions()->CopyFrom(orderConditionProto);
        }
    }
    if (order.conditionsCancelOrder) orderProto.set_conditionscancelorder(order.conditionsCancelOrder);
    if (order.conditionsIgnoreRth) orderProto.set_conditionsignorerth(order.conditionsIgnoreRth);

    if (!Utils::stringIsEmpty(order.modelCode)) orderProto.set_modelcode(order.modelCode);
    if (!Utils::stringIsEmpty(order.extOperator)) orderProto.set_extoperator(order.extOperator);

    orderProto.mutable_softdollartier()->CopyFrom(createSoftDollarTierProto(order));

    if (Utils::isValidValue(order.cashQty)) orderProto.set_cashqty(order.cashQty);
    if (!Utils::stringIsEmpty(order.mifid2DecisionMaker)) orderProto.set_mifid2decisionmaker(order.mifid2DecisionMaker);
    if (!Utils::stringIsEmpty(order.mifid2DecisionAlgo)) orderProto.set_mifid2decisionalgo(order.mifid2DecisionAlgo);
    if (!Utils::stringIsEmpty(order.mifid2ExecutionTrader)) orderProto.set_mifid2executiontrader(order.mifid2ExecutionTrader);
    if (!Utils::stringIsEmpty(order.mifid2ExecutionAlgo)) orderProto.set_mifid2executionalgo(order.mifid2ExecutionAlgo);
    if (order.dontUseAutoPriceForHedge) orderProto.set_dontuseautopriceforhedge(order.dontUseAutoPriceForHedge);
    if (order.isOmsContainer) orderProto.set_isomscontainer(order.isOmsContainer);
    if (order.discretionaryUpToLimitPrice) orderProto.set_discretionaryuptolimitprice(order.discretionaryUpToLimitPrice);
    if (Utils::isValidValue(order.usePriceMgmtAlgo)) orderProto.set_usepricemgmtalgo(order.usePriceMgmtAlgo);
    if (Utils::isValidValue(order.duration)) orderProto.set_duration(order.duration);
    if (Utils::isValidValue(order.postToAts)) orderProto.set_posttoats(order.postToAts);
    if (!Utils::stringIsEmpty(order.advancedErrorOverride)) orderProto.set_advancederroroverride(order.advancedErrorOverride);
    if (!Utils::stringIsEmpty(order.manualOrderTime)) orderProto.set_manualordertime(order.manualOrderTime);
    if (Utils::isValidValue(order.minTradeQty)) orderProto.set_mintradeqty(order.minTradeQty);
    if (Utils::isValidValue(order.minCompeteSize)) orderProto.set_mincompetesize(order.minCompeteSize);
    if (Utils::isValidValue(order.competeAgainstBestOffset)) orderProto.set_competeagainstbestoffset(order.competeAgainstBestOffset);
    if (Utils::isValidValue(order.midOffsetAtWhole)) orderProto.set_midoffsetatwhole(order.midOffsetAtWhole);
    if (Utils::isValidValue(order.midOffsetAtHalf)) orderProto.set_midoffsetathalf(order.midOffsetAtHalf);
    if (!Utils::stringIsEmpty(order.customerAccount)) orderProto.set_customeraccount(order.customerAccount);
    if (order.professionalCustomer) orderProto.set_professionalcustomer(order.professionalCustomer);
    if (!Utils::stringIsEmpty(order.bondAccruedInterest)) orderProto.set_bondaccruedinterest(order.bondAccruedInterest);
    if (order.includeOvernight) orderProto.set_includeovernight(order.includeOvernight);
    if (Utils::isValidValue(order.manualOrderIndicator)) orderProto.set_manualorderindicator(order.manualOrderIndicator);
    if (!Utils::stringIsEmpty(order.submitter)) orderProto.set_submitter(order.submitter);
    if (order.autoCancelParent) orderProto.set_autocancelparent(order.autoCancelParent);
    if (order.imbalanceOnly) orderProto.set_imbalanceonly(order.imbalanceOnly);

    return orderProto;
}

std::list<protobuf::OrderCondition> EClientUtils::createConditionsProto(Order order) {
    std::list<protobuf::OrderCondition> orderConditionList;
    if (order.conditions.size() > 0) {
        for (std::shared_ptr<OrderCondition> orderCondition : order.conditions) {
            OrderCondition::OrderConditionType type = orderCondition->type();

            protobuf::OrderCondition orderConditionProto;
            switch (type) {
            case OrderCondition::OrderConditionType::Price:
                orderConditionProto = createPriceConditionProto(*orderCondition);
                break;
            case OrderCondition::OrderConditionType::Time:
                orderConditionProto = createTimeConditionProto(*orderCondition);
                break;
            case OrderCondition::OrderConditionType::Margin:
                orderConditionProto = createMarginConditionProto(*orderCondition);
                break;
            case OrderCondition::OrderConditionType::Execution:
                orderConditionProto = createExecutionConditionProto(*orderCondition);
                break;
            case OrderCondition::OrderConditionType::Volume:
                orderConditionProto = createVolumeConditionProto(*orderCondition);
                break;
            case OrderCondition::OrderConditionType::PercentChange:
                orderConditionProto = createPercentChangeConditionProto(*orderCondition);
                break;
            }
            orderConditionList.push_back(orderConditionProto);
        }
    }
    return orderConditionList;
}

protobuf::OrderCondition EClientUtils::createOrderConditionProto(OrderCondition& condition) {
    int type = condition.type();
    bool isConjunctionConnection = condition.conjunctionConnection();
    protobuf::OrderCondition orderConditionProto;
    if (Utils::isValidValue(type)) orderConditionProto.set_type(type);
    orderConditionProto.set_isconjunctionconnection(isConjunctionConnection);
    return orderConditionProto;
}

protobuf::OrderCondition EClientUtils::createOperatorConditionProto(OrderCondition& condition) {
    protobuf::OrderCondition orderConditionProto = createOrderConditionProto(condition);
    OperatorCondition* operatorCondition = dynamic_cast<OperatorCondition*>(&condition);
    bool isMore = operatorCondition->isMore();
    protobuf::OrderCondition operatorConditionProto;
    operatorConditionProto.MergeFrom(orderConditionProto);
    operatorConditionProto.set_ismore(isMore);
    return operatorConditionProto;
}

protobuf::OrderCondition EClientUtils::createContractConditionProto(OrderCondition& condition) {
    protobuf::OrderCondition orderConditionProto = createOperatorConditionProto(condition);
    ContractCondition* contractCondition = dynamic_cast<ContractCondition*>(&condition);
    int conId = contractCondition->conId();
    std::string exchange = contractCondition->exchange();
    protobuf::OrderCondition contractConditionProto;
    contractConditionProto.MergeFrom(orderConditionProto);
    if (Utils::isValidValue(conId)) contractConditionProto.set_conid(conId);
    if (!Utils::stringIsEmpty(exchange)) contractConditionProto.set_exchange(exchange);
    return contractConditionProto;
}

protobuf::OrderCondition EClientUtils::createPriceConditionProto(OrderCondition& condition) {
    protobuf::OrderCondition orderConditionProto = createContractConditionProto(condition);
    PriceCondition* priceCondition = dynamic_cast< PriceCondition*>(&condition);
    double price = priceCondition->price();
    int triggerMethod = priceCondition->triggerMethod();
    protobuf::OrderCondition priceConditionProto;
    priceConditionProto.MergeFrom(orderConditionProto);
    if (Utils::isValidValue(price)) priceConditionProto.set_price(price);
    if (Utils::isValidValue(triggerMethod)) priceConditionProto.set_triggermethod(triggerMethod);
    return priceConditionProto;
}

protobuf::OrderCondition EClientUtils::createTimeConditionProto(OrderCondition& condition) {
    protobuf::OrderCondition operatorConditionProto = createOperatorConditionProto(condition);
    TimeCondition* timeCondition = dynamic_cast<TimeCondition*>(&condition);
    std::string time = timeCondition->time();
    protobuf::OrderCondition timeConditionProto;
    timeConditionProto.MergeFrom(operatorConditionProto);
    if (!Utils::stringIsEmpty(time)) timeConditionProto.set_time(time);
    return timeConditionProto;
}

protobuf::OrderCondition EClientUtils::createMarginConditionProto(OrderCondition& condition) {
    protobuf::OrderCondition operatorConditionProto = createOperatorConditionProto(condition);
    MarginCondition* marginCondition = dynamic_cast<MarginCondition*>(&condition);
    int percent = marginCondition->percent();
    protobuf::OrderCondition marginConditionProto;
    marginConditionProto.MergeFrom(operatorConditionProto);
    if (Utils::isValidValue(percent)) marginConditionProto.set_percent(percent);
    return marginConditionProto;
}

protobuf::OrderCondition EClientUtils::createExecutionConditionProto(OrderCondition& condition) {
    protobuf::OrderCondition orderConditionProto = createOrderConditionProto(condition);
    ExecutionCondition* executionCondition = dynamic_cast<ExecutionCondition*>(&condition);
    std::string secType = executionCondition->secType();
    std::string exchange = executionCondition->exchange();
    std::string symbol = executionCondition->symbol();
    protobuf::OrderCondition executionConditionProto;
    executionConditionProto.MergeFrom(orderConditionProto);
    if (!Utils::stringIsEmpty(secType)) executionConditionProto.set_sectype(secType);
    if (!Utils::stringIsEmpty(exchange)) executionConditionProto.set_exchange(exchange);
    if (!Utils::stringIsEmpty(symbol)) executionConditionProto.set_symbol(symbol);
    return executionConditionProto;
}

protobuf::OrderCondition EClientUtils::createVolumeConditionProto(OrderCondition& condition) {
    protobuf::OrderCondition orderConditionProto = createContractConditionProto(condition);
    VolumeCondition* volumeCondition = dynamic_cast<VolumeCondition*>(&condition);
    int volume = volumeCondition->volume();
    protobuf::OrderCondition volumeConditionProto;
    volumeConditionProto.MergeFrom(orderConditionProto);
    if (Utils::isValidValue(volume)) volumeConditionProto.set_volume(volume);
    return volumeConditionProto;
}

protobuf::OrderCondition EClientUtils::createPercentChangeConditionProto(OrderCondition& condition) {
    protobuf::OrderCondition orderConditionProto = createContractConditionProto(condition);
    PercentChangeCondition* percentChangeCondition = dynamic_cast<PercentChangeCondition*>(&condition);
    double changePercent = percentChangeCondition->changePercent();
    protobuf::OrderCondition percentChangeConditionProto;
    percentChangeConditionProto.MergeFrom(orderConditionProto);
    if (Utils::isValidValue(changePercent)) percentChangeConditionProto.set_changepercent(changePercent);
    return percentChangeConditionProto;
}

protobuf::SoftDollarTier EClientUtils::createSoftDollarTierProto(Order order) {
    SoftDollarTier tier = order.softDollarTier;
    protobuf::SoftDollarTier softDollarTierProto;
    if (!Utils::stringIsEmpty(tier.name())) softDollarTierProto.set_name(tier.name());
    if (!Utils::stringIsEmpty(tier.val())) softDollarTierProto.set_value(tier.val());
    if (!Utils::stringIsEmpty(tier.displayName())) softDollarTierProto.set_displayname(tier.displayName());
    return softDollarTierProto;
}

std::map<std::string, std::string> EClientUtils::createStringStringMap(TagValueListSPtr tagValueListSPtr) {
    std::map<std::string, std::string> stringStringMap;
    const TagValueList* tagValueList = tagValueListSPtr.get();
    const int tagValueListCount = tagValueList ? tagValueList->size() : 0;
    if (tagValueListCount > 0) {
        for (int i = 0; i < tagValueListCount; ++i) {
            const TagValue* tagValue = ((*tagValueList)[i]).get();
            stringStringMap.insert(std::pair<std::string, std::string>(tagValue->tag, tagValue->value));
        }
    }
    return stringStringMap;
}

protobuf::Contract EClientUtils::createContractProto(const Contract& contract, const Order& order) {
    protobuf::Contract contractProto;
    if (Utils::isValidValue(contract.conId)) contractProto.set_conid(contract.conId);
    if (!Utils::stringIsEmpty(contract.symbol)) contractProto.set_symbol(contract.symbol);
    if (!Utils::stringIsEmpty(contract.secType)) contractProto.set_sectype(contract.secType);
    if (!Utils::stringIsEmpty(contract.lastTradeDateOrContractMonth)) contractProto.set_lasttradedateorcontractmonth(contract.lastTradeDateOrContractMonth);
    if (Utils::isValidValue(contract.strike)) contractProto.set_strike(contract.strike);
    if (!Utils::stringIsEmpty(contract.right)) contractProto.set_right(contract.right);
    if (!Utils::stringIsEmpty(contract.multiplier)) contractProto.set_multiplier(atof(contract.multiplier.c_str()));
    if (!Utils::stringIsEmpty(contract.exchange)) contractProto.set_exchange(contract.exchange);
    if (!Utils::stringIsEmpty(contract.primaryExchange)) contractProto.set_primaryexch(contract.primaryExchange);
    if (!Utils::stringIsEmpty(contract.currency)) contractProto.set_currency(contract.currency);
    if (!Utils::stringIsEmpty(contract.localSymbol)) contractProto.set_localsymbol(contract.localSymbol);
    if (!Utils::stringIsEmpty(contract.tradingClass)) contractProto.set_tradingclass(contract.tradingClass);
    if (!Utils::stringIsEmpty(contract.secIdType)) contractProto.set_secidtype(contract.secIdType);
    if (!Utils::stringIsEmpty(contract.secId)) contractProto.set_secid(contract.secId);
    if (contract.includeExpired) contractProto.set_includeexpired(contract.includeExpired);
    if (!Utils::stringIsEmpty(contract.comboLegsDescrip)) contractProto.set_combolegsdescrip(contract.comboLegsDescrip);
    if (!Utils::stringIsEmpty(contract.description)) contractProto.set_description(contract.description);
    if (!Utils::stringIsEmpty(contract.issuerId)) contractProto.set_issuerid(contract.issuerId);

    std::list<protobuf::ComboLeg> comboLegProtoList = createComboLegProtoList(contract, order);
    if (!comboLegProtoList.empty()) {
        for (protobuf::ComboLeg comboLegProto : comboLegProtoList) {
            contractProto.add_combolegs()->CopyFrom(comboLegProto);
        }
    }

    protobuf::DeltaNeutralContract* deltaNeutralContractProto = createDeltaNeutralContractProto(contract);
    if (deltaNeutralContractProto != NULL) {
        contractProto.mutable_deltaneutralcontract()->CopyFrom(*deltaNeutralContractProto);
    }

    return contractProto;
}

protobuf::DeltaNeutralContract* EClientUtils::createDeltaNeutralContractProto(const Contract& contract) {
    if (contract.deltaNeutralContract == NULL) {
        return NULL;
    }
    DeltaNeutralContract* deltaNeutralContract = contract.deltaNeutralContract;
    protobuf::DeltaNeutralContract* deltaNeutralContractProto = new protobuf::DeltaNeutralContract();
    if (Utils::isValidValue(deltaNeutralContract->conId)) deltaNeutralContractProto->set_conid(deltaNeutralContract->conId);
    if (Utils::isValidValue(deltaNeutralContract->delta)) deltaNeutralContractProto->set_delta(deltaNeutralContract->delta);
    if (Utils::isValidValue(deltaNeutralContract->price)) deltaNeutralContractProto->set_price(deltaNeutralContract->price);
    return deltaNeutralContractProto;
}

std::list<protobuf::ComboLeg> EClientUtils::createComboLegProtoList(const Contract& contract, const Order& order) {
    std::list<protobuf::ComboLeg> comboLegProtoList;

    const Contract::ComboLegList* const comboLegs = contract.comboLegs.get();
    const int comboLegsCount = comboLegs ? comboLegs->size() : 0;
    const Order::OrderComboLegList* const orderComboLegs = order.orderComboLegs.get();
    const int orderComboLegsCount = orderComboLegs ? orderComboLegs->size() : 0;

    for (int i = 0; i < comboLegsCount; ++i) {
        const ComboLeg* comboLeg = ((*comboLegs)[i]).get();
        assert(comboLeg);
        double perLegPrice = UNSET_DOUBLE;
        if (i < orderComboLegsCount) {
            const OrderComboLeg* orderComboLeg = ((*orderComboLegs)[i]).get();
            assert(orderComboLeg);
            perLegPrice = orderComboLeg->price;
        }

        protobuf::ComboLeg comboLegProto = createComboLegProto(*comboLeg, perLegPrice);
        comboLegProtoList.push_back(comboLegProto);
    }

    return comboLegProtoList;
}

protobuf::ComboLeg EClientUtils::createComboLegProto(const ComboLeg& comboLeg, double perLegPrice) {
    protobuf::ComboLeg comboLegProto;
    if (Utils::isValidValue(comboLeg.conId)) comboLegProto.set_conid(comboLeg.conId);
    if (Utils::isValidValue(comboLeg.ratio)) comboLegProto.set_ratio(comboLeg.ratio);
    if (!Utils::stringIsEmpty(comboLeg.action)) comboLegProto.set_action(comboLeg.action);
    if (!Utils::stringIsEmpty(comboLeg.exchange)) comboLegProto.set_exchange(comboLeg.exchange);
    if (Utils::isValidValue(comboLeg.openClose)) comboLegProto.set_openclose(comboLeg.openClose);
    if (Utils::isValidValue(comboLeg.shortSaleSlot)) comboLegProto.set_shortsalesslot(comboLeg.shortSaleSlot);
    if (!Utils::stringIsEmpty(comboLeg.designatedLocation)) comboLegProto.set_designatedlocation(comboLeg.designatedLocation);
    if (Utils::isValidValue(comboLeg.exemptCode)) comboLegProto.set_exemptcode(comboLeg.exemptCode);
    if (Utils::isValidValue(perLegPrice)) comboLegProto.set_perlegprice(perLegPrice);
    return comboLegProto;
}

protobuf::CancelOrderRequest EClientUtils::createCancelOrderRequestProto(OrderId orderId, const OrderCancel& orderCancel) {
    protobuf::CancelOrderRequest cancelOrderRequestProto;
    if (Utils::isValidValue(orderId)) cancelOrderRequestProto.set_orderid(orderId);
    cancelOrderRequestProto.mutable_ordercancel()->CopyFrom(createOrderCancelProto(orderCancel));
    return cancelOrderRequestProto;
}

protobuf::GlobalCancelRequest EClientUtils::createGlobalCancelRequestProto(const OrderCancel& orderCancel) {
    protobuf::GlobalCancelRequest globalCancelRequestProto;
    globalCancelRequestProto.mutable_ordercancel()->CopyFrom(createOrderCancelProto(orderCancel));
    return globalCancelRequestProto;
}

protobuf::OrderCancel EClientUtils::createOrderCancelProto(const OrderCancel& orderCancel) {
    protobuf::OrderCancel orderCancelProto;
    if (!Utils::stringIsEmpty(orderCancel.manualOrderCancelTime)) orderCancelProto.set_manualordercanceltime(orderCancel.manualOrderCancelTime.c_str());
    if (!Utils::stringIsEmpty(orderCancel.extOperator)) orderCancelProto.set_extoperator(orderCancel.extOperator.c_str());
    if (Utils::isValidValue(orderCancel.manualOrderIndicator)) orderCancelProto.set_manualorderindicator(orderCancel.manualOrderIndicator);
    return orderCancelProto;;
}