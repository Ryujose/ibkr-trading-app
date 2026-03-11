/* Copyright (C) 2025 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

package com.ib.client;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

import com.google.protobuf.InvalidProtocolBufferException;
import com.ib.client.protobuf.CancelOrderRequestProto;
import com.ib.client.protobuf.ComboLegProto;
import com.ib.client.protobuf.ContractProto;
import com.ib.client.protobuf.DeltaNeutralContractProto;
import com.ib.client.protobuf.ExecutionFilterProto;
import com.ib.client.protobuf.ExecutionRequestProto;
import com.ib.client.protobuf.GlobalCancelRequestProto;
import com.ib.client.protobuf.OrderCancelProto;
import com.ib.client.protobuf.OrderConditionProto;
import com.ib.client.protobuf.OrderProto;
import com.ib.client.protobuf.PlaceOrderRequestProto;
import com.ib.client.protobuf.SoftDollarTierProto;

public class EClientUtils {

    public static ExecutionRequestProto.ExecutionRequest createExecutionRequestProto(int reqId, ExecutionFilter filter) {
        ExecutionFilterProto.ExecutionFilter.Builder executionFilterBuilder = ExecutionFilterProto.ExecutionFilter.newBuilder();
        if (Util.isValidValue(filter.clientId())) executionFilterBuilder.setClientId(filter.clientId());
        if (!Util.StringIsEmpty(filter.acctCode())) executionFilterBuilder.setAcctCode(filter.acctCode());
        if (!Util.StringIsEmpty(filter.time())) executionFilterBuilder.setTime(filter.time());
        if (!Util.StringIsEmpty(filter.symbol())) executionFilterBuilder.setSymbol(filter.symbol());
        if (!Util.StringIsEmpty(filter.secType())) executionFilterBuilder.setSecType(filter.secType());
        if (!Util.StringIsEmpty(filter.exchange())) executionFilterBuilder.setExchange(filter.exchange());
        if (!Util.StringIsEmpty(filter.side())) executionFilterBuilder.setSide(filter.side());
        if (Util.isValidValue(filter.lastNDays())) executionFilterBuilder.setLastNDays(filter.lastNDays());
        if (filter.specificDates() != null) filter.specificDates().stream().forEach(specificDate -> executionFilterBuilder.addSpecificDates(specificDate));
        ExecutionRequestProto.ExecutionRequest.Builder executionRequestBuilder = ExecutionRequestProto.ExecutionRequest.newBuilder();
        if (Util.isValidValue(reqId)) executionRequestBuilder.setReqId(reqId);
        executionRequestBuilder.setExecutionFilter(executionFilterBuilder.build());
        return executionRequestBuilder.build();
    }
    
    public static PlaceOrderRequestProto.PlaceOrderRequest createPlaceOrderRequestProto(int orderId, Contract contract, Order order) throws EClientException {
        PlaceOrderRequestProto.PlaceOrderRequest.Builder placeOrderRequestBuilder = PlaceOrderRequestProto.PlaceOrderRequest.newBuilder();

        if (Util.isValidValue(orderId)) placeOrderRequestBuilder.setOrderId(orderId);

        ContractProto.Contract contractProto = createContractProto(contract, order);
        if (contractProto != null) placeOrderRequestBuilder.setContract(contractProto);

        OrderProto.Order orderProto = createOrderProto(order);
        if (orderProto != null) placeOrderRequestBuilder.setOrder(orderProto);

        return placeOrderRequestBuilder.build();
    }

    public static OrderProto.Order createOrderProto(Order order) throws EClientException {
        OrderProto.Order.Builder orderBuilder = OrderProto.Order.newBuilder();
        if (Util.isValidValue(order.clientId())) orderBuilder.setClientId(order.clientId());
        if (Util.isValidValue(order.permId())) orderBuilder.setPermId(order.permId());
        if (Util.isValidValue(order.parentId())) orderBuilder.setParentId(order.parentId());
        if (!Util.StringIsEmpty(order.getAction())) orderBuilder.setAction(order.getAction());
        if (Util.isValidValue(order.totalQuantity())) orderBuilder.setTotalQuantity(order.totalQuantity().toString());
        if (Util.isValidValue(order.displaySize())) orderBuilder.setDisplaySize(order.displaySize());
        if (!Util.StringIsEmpty(order.getOrderType())) orderBuilder.setOrderType(order.getOrderType());
        if (Util.isValidValue(order.lmtPrice())) orderBuilder.setLmtPrice(order.lmtPrice());
        if (Util.isValidValue(order.auxPrice())) orderBuilder.setAuxPrice(order.auxPrice());
        if (!Util.StringIsEmpty(order.getTif())) orderBuilder.setTif(order.getTif());
        if (!Util.StringIsEmpty(order.account())) orderBuilder.setAccount(order.account());
        if (!Util.StringIsEmpty(order.settlingFirm())) orderBuilder.setSettlingFirm(order.settlingFirm());
        if (!Util.StringIsEmpty(order.clearingAccount())) orderBuilder.setClearingAccount(order.clearingAccount());
        if (!Util.StringIsEmpty(order.clearingIntent())) orderBuilder.setClearingIntent(order.clearingIntent());
        if (order.allOrNone()) orderBuilder.setAllOrNone(order.allOrNone());
        if (order.blockOrder()) orderBuilder.setBlockOrder(order.blockOrder());
        if (order.hidden()) orderBuilder.setHidden(order.hidden());
        if (order.outsideRth()) orderBuilder.setOutsideRth(order.outsideRth());
        if (order.sweepToFill()) orderBuilder.setSweepToFill(order.sweepToFill());
        if (Util.isValidValue(order.percentOffset())) orderBuilder.setPercentOffset(order.percentOffset());
        if (Util.isValidValue(order.trailingPercent())) orderBuilder.setTrailingPercent(order.trailingPercent());
        if (Util.isValidValue(order.trailStopPrice())) orderBuilder.setTrailStopPrice(order.trailStopPrice());
        if (Util.isValidValue(order.minQty())) orderBuilder.setMinQty(order.minQty());
        if (!Util.StringIsEmpty(order.goodAfterTime())) orderBuilder.setGoodAfterTime(order.goodAfterTime());
        if (!Util.StringIsEmpty(order.goodTillDate())) orderBuilder.setGoodTillDate(order.goodTillDate());
        if (!Util.StringIsEmpty(order.ocaGroup())) orderBuilder.setOcaGroup(order.ocaGroup());
        if (!Util.StringIsEmpty(order.orderRef())) orderBuilder.setOrderRef(order.orderRef());
        if (!Util.StringIsEmpty(order.getRule80A())) orderBuilder.setRule80A(order.getRule80A());
        if (Util.isValidValue(order.getOcaType())) orderBuilder.setOcaType(order.getOcaType());
        if (Util.isValidValue(order.getTriggerMethod())) orderBuilder.setTriggerMethod(order.getTriggerMethod());
        if (!Util.StringIsEmpty(order.activeStartTime())) orderBuilder.setActiveStartTime(order.activeStartTime());
        if (!Util.StringIsEmpty(order.activeStopTime())) orderBuilder.setActiveStopTime(order.activeStopTime());
        if (!Util.StringIsEmpty(order.faGroup())) orderBuilder.setFaGroup(order.faGroup());
        if (!Util.StringIsEmpty(order.getFaMethod())) orderBuilder.setFaMethod(order.getFaMethod());
        if (!Util.StringIsEmpty(order.faPercentage())) orderBuilder.setFaPercentage(order.faPercentage());
        if (Util.isValidValue(order.volatility()))  orderBuilder.setVolatility(order.volatility());
        if (Util.isValidValue(order.getVolatilityType())) orderBuilder.setVolatilityType(order.getVolatilityType());
        if (Util.isValidValue(order.continuousUpdate())) orderBuilder.setContinuousUpdate(order.continuousUpdate() == 1);
        if (Util.isValidValue(order.getReferencePriceType())) orderBuilder.setReferencePriceType(order.getReferencePriceType());
        if (!Util.StringIsEmpty(order.getDeltaNeutralOrderType())) orderBuilder.setDeltaNeutralOrderType(order.getDeltaNeutralOrderType());
        if (Util.isValidValue(order.deltaNeutralAuxPrice())) orderBuilder.setDeltaNeutralAuxPrice(order.deltaNeutralAuxPrice());
        if (Util.isValidValue(order.deltaNeutralConId())) orderBuilder.setDeltaNeutralConId(order.deltaNeutralConId());
        if (!Util.StringIsEmpty(order.deltaNeutralOpenClose())) orderBuilder.setDeltaNeutralOpenClose(order.deltaNeutralOpenClose());
        if (order.deltaNeutralShortSale()) orderBuilder.setDeltaNeutralShortSale(order.deltaNeutralShortSale());
        if (Util.isValidValue(order.deltaNeutralShortSaleSlot())) orderBuilder.setDeltaNeutralShortSaleSlot(order.deltaNeutralShortSaleSlot());
        if (!Util.StringIsEmpty(order.deltaNeutralDesignatedLocation())) orderBuilder.setDeltaNeutralDesignatedLocation(order.deltaNeutralDesignatedLocation());
        if (Util.isValidValue(order.scaleInitLevelSize())) orderBuilder.setScaleInitLevelSize(order.scaleInitLevelSize());
        if (Util.isValidValue(order.scaleSubsLevelSize())) orderBuilder.setScaleSubsLevelSize(order.scaleSubsLevelSize());
        if (Util.isValidValue(order.scalePriceIncrement())) orderBuilder.setScalePriceIncrement(order.scalePriceIncrement());
        if (Util.isValidValue(order.scalePriceAdjustValue())) orderBuilder.setScalePriceAdjustValue(order.scalePriceAdjustValue());
        if (Util.isValidValue(order.scalePriceAdjustInterval())) orderBuilder.setScalePriceAdjustInterval(order.scalePriceAdjustInterval());
        if (Util.isValidValue(order.scaleProfitOffset())) orderBuilder.setScaleProfitOffset(order.scaleProfitOffset());
        if (order.scaleAutoReset()) orderBuilder.setScaleAutoReset(order.scaleAutoReset());
        if (Util.isValidValue(order.scaleInitPosition())) orderBuilder.setScaleInitPosition(order.scaleInitPosition());
        if (Util.isValidValue(order.scaleInitFillQty())) orderBuilder.setScaleInitFillQty(order.scaleInitFillQty());
        if (order.scaleRandomPercent()) orderBuilder.setScaleRandomPercent(order.scaleRandomPercent());
        if (!Util.StringIsEmpty(order.scaleTable())) orderBuilder.setScaleTable(order.scaleTable());
        if (!Util.StringIsEmpty(order.getHedgeType())) orderBuilder.setHedgeType(order.getHedgeType());
        if (!Util.StringIsEmpty(order.hedgeParam())) orderBuilder.setHedgeParam(order.hedgeParam());

        if (!Util.StringIsEmpty(order.getAlgoStrategy())) orderBuilder.setAlgoStrategy(order.getAlgoStrategy());
        if (order.algoParams() != null && !order.algoParams().isEmpty()) {
            Map<String, String> algoParams = order.algoParams().stream().collect(Collectors.toMap(e -> e.m_tag, e -> e.m_value));
            orderBuilder.putAllAlgoParams(algoParams);
        }
        if (!Util.StringIsEmpty(order.algoId())) orderBuilder.setAlgoId(order.algoId());

        if (order.smartComboRoutingParams() != null && !order.smartComboRoutingParams().isEmpty()) {
            Map<String, String> smartComboRoutingParams = order.smartComboRoutingParams().stream().collect(Collectors.toMap(e -> e.m_tag, e -> e.m_value)); 
            orderBuilder.putAllSmartComboRoutingParams(smartComboRoutingParams);
        }

        if (order.whatIf()) orderBuilder.setWhatIf(order.whatIf());
        if (order.transmit()) orderBuilder.setTransmit(order.transmit());
        if (order.overridePercentageConstraints()) orderBuilder.setOverridePercentageConstraints(order.overridePercentageConstraints());
        if (!Util.StringIsEmpty(order.openClose())) orderBuilder.setOpenClose(order.openClose());
        if (Util.isValidValue(order.origin())) orderBuilder.setOrigin(order.origin());
        if (Util.isValidValue(order.shortSaleSlot())) orderBuilder.setShortSaleSlot(order.shortSaleSlot());
        if (!Util.StringIsEmpty(order.designatedLocation())) orderBuilder.setDesignatedLocation(order.designatedLocation());
        if (Util.isValidValue(order.exemptCode())) orderBuilder.setExemptCode(order.exemptCode());
        if (!Util.StringIsEmpty(order.deltaNeutralSettlingFirm())) orderBuilder.setDeltaNeutralSettlingFirm(order.deltaNeutralSettlingFirm());
        if (!Util.StringIsEmpty(order.deltaNeutralClearingAccount())) orderBuilder.setDeltaNeutralClearingAccount(order.deltaNeutralClearingAccount());
        if (!Util.StringIsEmpty(order.deltaNeutralClearingIntent())) orderBuilder.setDeltaNeutralClearingIntent(order.deltaNeutralClearingIntent());
        if (Util.isValidValue(order.discretionaryAmt())) orderBuilder.setDiscretionaryAmt(order.discretionaryAmt());
        if (order.optOutSmartRouting()) orderBuilder.setOptOutSmartRouting(order.optOutSmartRouting());
        if (Util.isValidValue(order.exemptCode())) orderBuilder.setExemptCode(order.exemptCode());
        if (Util.isValidValue(order.startingPrice())) orderBuilder.setStartingPrice(order.startingPrice());
        if (Util.isValidValue(order.stockRefPrice())) orderBuilder.setStockRefPrice(order.stockRefPrice());
        if (Util.isValidValue(order.delta())) orderBuilder.setDelta(order.delta());
        if (Util.isValidValue(order.stockRangeLower())) orderBuilder.setStockRangeLower(order.stockRangeLower());
        if (Util.isValidValue(order.stockRangeUpper())) orderBuilder.setStockRangeUpper(order.stockRangeUpper());
        if (order.notHeld()) orderBuilder.setNotHeld(order.notHeld());

        if (order.orderMiscOptions() != null && !order.orderMiscOptions().isEmpty()) {
            Map<String, String> orderMiscOptions = order.orderMiscOptions().stream().collect(Collectors.toMap(e -> e.m_tag, e -> e.m_value)); 
            orderBuilder.putAllOrderMiscOptions(orderMiscOptions);
        }

        if (order.solicited()) orderBuilder.setSolicited(order.solicited());
        if (order.randomizeSize()) orderBuilder.setRandomizeSize(order.randomizeSize());
        if (order.randomizePrice()) orderBuilder.setRandomizePrice(order.randomizePrice());
        if (Util.isValidValue(order.referenceContractId())) orderBuilder.setReferenceContractId(order.referenceContractId());
        if (Util.isValidValue(order.peggedChangeAmount())) orderBuilder.setPeggedChangeAmount(order.peggedChangeAmount());
        if (order.isPeggedChangeAmountDecrease()) orderBuilder.setIsPeggedChangeAmountDecrease(order.isPeggedChangeAmountDecrease());
        if (Util.isValidValue(order.referenceChangeAmount())) orderBuilder.setReferenceChangeAmount(order.referenceChangeAmount());
        if (!Util.StringIsEmpty(order.referenceExchangeId())) orderBuilder.setReferenceExchangeId(order.referenceExchangeId());
        if (order.adjustedOrderType() != null && !Util.StringIsEmpty(order.adjustedOrderType().getApiString())) orderBuilder.setAdjustedOrderType(order.adjustedOrderType().getApiString());
        if (Util.isValidValue(order.triggerPrice())) orderBuilder.setTriggerPrice(order.triggerPrice());
        if (Util.isValidValue(order.adjustedStopPrice())) orderBuilder.setAdjustedStopPrice(order.adjustedStopPrice());
        if (Util.isValidValue(order.adjustedStopLimitPrice())) orderBuilder.setAdjustedStopLimitPrice(order.adjustedStopLimitPrice());
        if (Util.isValidValue(order.adjustedTrailingAmount())) orderBuilder.setAdjustedTrailingAmount(order.adjustedTrailingAmount());
        if (Util.isValidValue(order.adjustableTrailingUnit())) orderBuilder.setAdjustableTrailingUnit(order.adjustableTrailingUnit());
        if (Util.isValidValue(order.lmtPriceOffset())) orderBuilder.setLmtPriceOffset(order.lmtPriceOffset());

        List<OrderConditionProto.OrderCondition> orderConditionList = createConditionsProto(order);
        if (!orderConditionList.isEmpty()) orderBuilder.addAllConditions(orderConditionList);
        if (order.conditionsCancelOrder()) orderBuilder.setConditionsCancelOrder(order.conditionsCancelOrder());
        if (order.conditionsIgnoreRth()) orderBuilder.setConditionsIgnoreRth(order.conditionsIgnoreRth());

        if (!Util.StringIsEmpty(order.modelCode())) orderBuilder.setModelCode(order.modelCode());
        if (!Util.StringIsEmpty(order.extOperator())) orderBuilder.setExtOperator(order.extOperator());

        SoftDollarTierProto.SoftDollarTier softDollarTier = createSoftDollarTierProto(order);
        if (softDollarTier != null) orderBuilder.setSoftDollarTier(softDollarTier);

        if (Util.isValidValue(order.cashQty())) orderBuilder.setCashQty(order.cashQty());
        if (!Util.StringIsEmpty(order.mifid2DecisionMaker())) orderBuilder.setMifid2DecisionMaker(order.mifid2DecisionMaker());
        if (!Util.StringIsEmpty(order.mifid2DecisionAlgo())) orderBuilder.setMifid2DecisionAlgo(order.mifid2DecisionAlgo());
        if (!Util.StringIsEmpty(order.mifid2ExecutionTrader())) orderBuilder.setMifid2ExecutionTrader(order.mifid2ExecutionTrader());
        if (!Util.StringIsEmpty(order.mifid2ExecutionAlgo())) orderBuilder.setMifid2ExecutionAlgo(order.mifid2ExecutionAlgo());
        if (order.dontUseAutoPriceForHedge()) orderBuilder.setDontUseAutoPriceForHedge(order.dontUseAutoPriceForHedge());
        if (order.isOmsContainer()) orderBuilder.setIsOmsContainer(order.isOmsContainer());
        if (order.discretionaryUpToLimitPrice()) orderBuilder.setDiscretionaryUpToLimitPrice(order.discretionaryUpToLimitPrice());
        if (order.usePriceMgmtAlgo() != null) orderBuilder.setUsePriceMgmtAlgo(order.usePriceMgmtAlgo() ? 1 : 0);
        if (Util.isValidValue(order.duration())) orderBuilder.setDuration(order.duration());
        if (Util.isValidValue(order.postToAts())) orderBuilder.setPostToAts(order.postToAts());
        if (!Util.StringIsEmpty(order.advancedErrorOverride())) orderBuilder.setAdvancedErrorOverride(order.advancedErrorOverride());
        if (!Util.StringIsEmpty(order.manualOrderTime())) orderBuilder.setManualOrderTime(order.manualOrderTime());
        if (Util.isValidValue(order.minTradeQty())) orderBuilder.setMinTradeQty(order.minTradeQty());
        if (Util.isValidValue(order.minCompeteSize())) orderBuilder.setMinCompeteSize(order.minCompeteSize());
        if (Util.isValidValue(order.competeAgainstBestOffset()) || order.isCompeteAgainstBestOffsetUpToMid()) orderBuilder.setCompeteAgainstBestOffset(order.competeAgainstBestOffset());
        if (Util.isValidValue(order.midOffsetAtWhole())) orderBuilder.setMidOffsetAtWhole(order.midOffsetAtWhole());
        if (Util.isValidValue(order.midOffsetAtHalf())) orderBuilder.setMidOffsetAtHalf(order.midOffsetAtHalf());
        if (!Util.StringIsEmpty(order.customerAccount())) orderBuilder.setCustomerAccount(order.customerAccount());
        if (order.professionalCustomer()) orderBuilder.setProfessionalCustomer(order.professionalCustomer());
        if (!Util.StringIsEmpty(order.bondAccruedInterest())) orderBuilder.setBondAccruedInterest(order.bondAccruedInterest());
        if (order.includeOvernight()) orderBuilder.setIncludeOvernight(order.includeOvernight());
        if (Util.isValidValue(order.manualOrderIndicator())) orderBuilder.setManualOrderIndicator(order.manualOrderIndicator());
        if (!Util.StringIsEmpty(order.submitter())) orderBuilder.setSubmitter(order.submitter());
        if (order.autoCancelParent()) orderBuilder.setAutoCancelParent(order.autoCancelParent());
        if (order.imbalanceOnly()) orderBuilder.setImbalanceOnly(order.imbalanceOnly());

        return orderBuilder.build();
    }

    public static List<OrderConditionProto.OrderCondition> createConditionsProto(Order order) throws EClientException {
        List<OrderConditionProto.OrderCondition> orderConditionList = new ArrayList<OrderConditionProto.OrderCondition>();
        try {
            if (order.conditions() != null && !order.conditions().isEmpty()) {
                for (OrderCondition condition : order.conditions()) {
                    OrderConditionType type = condition.type();
                    OrderConditionProto.OrderCondition orderConditionProto = null;
                    switch(type) {
                        case Price:
                            orderConditionProto = createPriceConditionProto(condition);
                            break;
                        case Time:
                            orderConditionProto = createTimeConditionProto(condition);
                            break;
                        case Margin:
                            orderConditionProto = createMarginConditionProto(condition);
                            break;
                        case Execution:
                            orderConditionProto = createExecutionConditionProto(condition);
                            break;
                        case Volume:
                            orderConditionProto = createVolumeConditionProto(condition);
                            break;
                        case PercentChange:
                            orderConditionProto = createPercentChangeConditionProto(condition);
                            break;
                    }
                    if (orderConditionProto != null) {
                        orderConditionList.add(orderConditionProto);
                    }
                }
            }
        } catch (InvalidProtocolBufferException e) {
            throw new EClientException(EClientErrors.ERROR_ENCODING_PROTOBUF, "Error encoding conditions");
        }
        return orderConditionList;
    }

    private static OrderConditionProto.OrderCondition createOrderConditionProto(OrderCondition condition) {
        int type = condition.type().val();
        boolean isConjunctionConnection = condition.conjunctionConnection();
        OrderConditionProto.OrderCondition.Builder orderConditionBuilder = OrderConditionProto.OrderCondition.newBuilder();
        if (Util.isValidValue(type)) orderConditionBuilder.setType(type);
        orderConditionBuilder.setIsConjunctionConnection(isConjunctionConnection);
        return orderConditionBuilder.build();
    }

    private static OrderConditionProto.OrderCondition createOperatorConditionProto(OrderCondition condition) throws InvalidProtocolBufferException  {
        OrderConditionProto.OrderCondition orderConditionProto = createOrderConditionProto(condition);
        OperatorCondition operatorCondition = (OperatorCondition)condition; 
        boolean isMore = operatorCondition.isMore();
        OrderConditionProto.OrderCondition.Builder orderConditionBuilder = OrderConditionProto.OrderCondition.newBuilder();
        orderConditionBuilder.mergeFrom(orderConditionProto.toByteArray());
        orderConditionBuilder.setIsMore(isMore);
        return orderConditionBuilder.build();
    }

    private static OrderConditionProto.OrderCondition createContractConditionProto(OrderCondition condition) throws InvalidProtocolBufferException {
        OrderConditionProto.OrderCondition orderConditionProto = createOperatorConditionProto(condition);
        ContractCondition contractCondition = (ContractCondition)condition; 
        int conId = contractCondition.conId();
        String exchange = contractCondition.exchange();
        OrderConditionProto.OrderCondition.Builder orderConditionBuilder = OrderConditionProto.OrderCondition.newBuilder();
        orderConditionBuilder.mergeFrom(orderConditionProto.toByteArray());
        if (Util.isValidValue(conId)) orderConditionBuilder.setConId(conId);
        if (!Util.StringIsEmpty(exchange)) orderConditionBuilder.setExchange(exchange);
        return orderConditionBuilder.build();
    }

    private static OrderConditionProto.OrderCondition createPriceConditionProto(OrderCondition condition) throws InvalidProtocolBufferException {
        OrderConditionProto.OrderCondition orderConditionProto = createContractConditionProto(condition);
        PriceCondition priceCondition = (PriceCondition)condition; 
        double price = priceCondition.price();
        int triggerMethod = priceCondition.triggerMethod();
        OrderConditionProto.OrderCondition.Builder orderConditionBuilder = OrderConditionProto.OrderCondition.newBuilder();
        orderConditionBuilder.mergeFrom(orderConditionProto.toByteArray());
        if (Util.isValidValue(price)) orderConditionBuilder.setPrice(price);
        if (Util.isValidValue(triggerMethod)) orderConditionBuilder.setTriggerMethod(triggerMethod);
        return orderConditionBuilder.build();
    }

    private static OrderConditionProto.OrderCondition createTimeConditionProto(OrderCondition condition) throws InvalidProtocolBufferException {
        OrderConditionProto.OrderCondition operatorConditionProto = createOperatorConditionProto(condition);
        TimeCondition timeCondition = (TimeCondition)condition; 
        String time = timeCondition.time();
        OrderConditionProto.OrderCondition.Builder orderConditionBuilder = OrderConditionProto.OrderCondition.newBuilder();
        orderConditionBuilder.mergeFrom(operatorConditionProto.toByteArray());
        if (!Util.StringIsEmpty(time)) orderConditionBuilder.setTime(time);
        return orderConditionBuilder.build();
    }

    private static OrderConditionProto.OrderCondition createMarginConditionProto(OrderCondition condition) throws InvalidProtocolBufferException {
        OrderConditionProto.OrderCondition operatorConditionProto = createOperatorConditionProto(condition);
        MarginCondition marginCondition = (MarginCondition)condition; 
        int percent = marginCondition.percent();
        OrderConditionProto.OrderCondition.Builder orderConditionBuilder = OrderConditionProto.OrderCondition.newBuilder();
        orderConditionBuilder.mergeFrom(operatorConditionProto.toByteArray());
        if (Util.isValidValue(percent)) orderConditionBuilder.setPercent(percent);
        return orderConditionBuilder.build();
    }

    private static OrderConditionProto.OrderCondition createExecutionConditionProto(OrderCondition condition) throws InvalidProtocolBufferException  {
        OrderConditionProto.OrderCondition orderConditionProto = createOrderConditionProto(condition);
        ExecutionCondition executionCondition = (ExecutionCondition)condition; 
        String secType = executionCondition.secType();
        String exchange = executionCondition.exchange();
        String symbol = executionCondition.symbol();
        OrderConditionProto.OrderCondition.Builder orderConditionBuilder = OrderConditionProto.OrderCondition.newBuilder();
        orderConditionBuilder.mergeFrom(orderConditionProto.toByteArray());
        if (!Util.StringIsEmpty(secType)) orderConditionBuilder.setSecType(secType);
        if (!Util.StringIsEmpty(exchange)) orderConditionBuilder.setExchange(exchange);
        if (!Util.StringIsEmpty(symbol)) orderConditionBuilder.setSymbol(symbol);
        return orderConditionBuilder.build();
    }

    private static OrderConditionProto.OrderCondition createVolumeConditionProto(OrderCondition condition) throws InvalidProtocolBufferException {
        OrderConditionProto.OrderCondition orderConditionProto = createContractConditionProto(condition);
        VolumeCondition volumeCondition = (VolumeCondition)condition; 
        int volume = volumeCondition.volume();
        OrderConditionProto.OrderCondition.Builder orderConditionBuilder = OrderConditionProto.OrderCondition.newBuilder();
        orderConditionBuilder.mergeFrom(orderConditionProto.toByteArray());
        if (Util.isValidValue(volume)) orderConditionBuilder.setVolume(volume);
        return orderConditionBuilder.build();
    }

    private static OrderConditionProto.OrderCondition createPercentChangeConditionProto(OrderCondition condition) throws InvalidProtocolBufferException {
        OrderConditionProto.OrderCondition orderConditionProto = createContractConditionProto(condition);
        PercentChangeCondition percentChangeCondition = (PercentChangeCondition)condition; 
        double changePercent = percentChangeCondition.changePercent();
        OrderConditionProto.OrderCondition.Builder orderConditionBuilder = OrderConditionProto.OrderCondition.newBuilder();
        orderConditionBuilder.mergeFrom(orderConditionProto.toByteArray());
        if (Util.isValidValue(changePercent)) orderConditionBuilder.setChangePercent(changePercent);
        return orderConditionBuilder.build();
    }

    public static SoftDollarTierProto.SoftDollarTier createSoftDollarTierProto(Order order) {
        SoftDollarTier tier = order.softDollarTier();
        if (tier == null) {
            return null;
        }

        SoftDollarTierProto.SoftDollarTier.Builder softDollarTierBuilder = SoftDollarTierProto.SoftDollarTier.newBuilder();
        if (!Util.StringIsEmpty(tier.name())) softDollarTierBuilder.setName(tier.name());
        if (!Util.StringIsEmpty(tier.value())) softDollarTierBuilder.setValue(tier.value());
        if (!Util.StringIsEmpty(tier.displayName())) softDollarTierBuilder.setDisplayName(tier.displayName());
        return softDollarTierBuilder.build();
    }

    public static ContractProto.Contract createContractProto(Contract contract, Order order) {
        ContractProto.Contract.Builder contractBuilder = ContractProto.Contract.newBuilder();
        if (Util.isValidValue(contract.conid())) contractBuilder.setConId(contract.conid());
        if (!Util.StringIsEmpty(contract.symbol())) contractBuilder.setSymbol(contract.symbol());
        if (!Util.StringIsEmpty(contract.getSecType())) contractBuilder.setSecType(contract.getSecType());
        if (!Util.StringIsEmpty(contract.lastTradeDateOrContractMonth())) contractBuilder.setLastTradeDateOrContractMonth(contract.lastTradeDateOrContractMonth());
        if (Util.isValidValue(contract.strike())) contractBuilder.setStrike(contract.strike());
        if (!Util.StringIsEmpty(contract.getRight())) contractBuilder.setRight(contract.getRight());
        if (!Util.StringIsEmpty(contract.multiplier())) contractBuilder.setMultiplier(Double.parseDouble(contract.multiplier()));
        if (!Util.StringIsEmpty(contract.exchange())) contractBuilder.setExchange(contract.exchange());
        if (!Util.StringIsEmpty(contract.primaryExch())) contractBuilder.setPrimaryExch(contract.primaryExch());
        if (!Util.StringIsEmpty(contract.currency())) contractBuilder.setCurrency(contract.currency());
        if (!Util.StringIsEmpty(contract.localSymbol())) contractBuilder.setLocalSymbol(contract.localSymbol());
        if (!Util.StringIsEmpty(contract.tradingClass())) contractBuilder.setTradingClass(contract.tradingClass());
        if (!Util.StringIsEmpty(contract.getSecIdType())) contractBuilder.setSecIdType(contract.getSecIdType());
        if (!Util.StringIsEmpty(contract.secId())) contractBuilder.setSecId(contract.secId());
        if (contract.includeExpired()) contractBuilder.setIncludeExpired(contract.includeExpired());
        if (!Util.StringIsEmpty(contract.comboLegsDescrip())) contractBuilder.setComboLegsDescrip(contract.comboLegsDescrip());
        if (!Util.StringIsEmpty(contract.description())) contractBuilder.setDescription(contract.description());
        if (!Util.StringIsEmpty(contract.issuerId())) contractBuilder.setIssuerId(contract.issuerId());

        List<ComboLegProto.ComboLeg> comboLegProtoList = createComboLegProtoList(contract, order);
        if (comboLegProtoList != null) {
            contractBuilder.addAllComboLegs(comboLegProtoList);
        }
        DeltaNeutralContractProto.DeltaNeutralContract deltaNeutralContractProto = createDeltaNeutralContractProto(contract);
        if (deltaNeutralContractProto != null) {
            contractBuilder.setDeltaNeutralContract(deltaNeutralContractProto);
        }
        return contractBuilder.build();
    }

    public static DeltaNeutralContractProto.DeltaNeutralContract createDeltaNeutralContractProto(Contract contract) {
        if (contract.deltaNeutralContract() == null) {
            return null;
        }
        DeltaNeutralContract deltaNeutralContract = contract.deltaNeutralContract();
        DeltaNeutralContractProto.DeltaNeutralContract.Builder deltaNeutralContractBuilder = DeltaNeutralContractProto.DeltaNeutralContract.newBuilder();
        if (Util.isValidValue(deltaNeutralContract.conid())) deltaNeutralContractBuilder.setConId(deltaNeutralContract.conid());
        if (Util.isValidValue(deltaNeutralContract.delta())) deltaNeutralContractBuilder.setDelta(deltaNeutralContract.delta());
        if (Util.isValidValue(deltaNeutralContract.price())) deltaNeutralContractBuilder.setPrice(deltaNeutralContract.price());
        return deltaNeutralContractBuilder.build();
    }

    public static List<ComboLegProto.ComboLeg> createComboLegProtoList(Contract contract, Order order) {
        List<ComboLeg> comboLegs = contract.comboLegs();
        if (comboLegs == null || comboLegs.isEmpty()) {
            return null;
        }
        List<ComboLegProto.ComboLeg> comboLegProtoList = new ArrayList<ComboLegProto.ComboLeg>();
        for(int i = 0; i < comboLegs.size(); i++) {
            ComboLeg comboLeg = comboLegs.get(i);
            double perLegPrice = Double.MAX_VALUE;
            if (i < order.orderComboLegs().size()) {
                perLegPrice = order.orderComboLegs().get(i).price();
            }
            ComboLegProto.ComboLeg comboLegProto = createComboLegProto(comboLeg, perLegPrice);
            comboLegProtoList.add(comboLegProto);
        }
        return comboLegProtoList;
    }

    public static ComboLegProto.ComboLeg createComboLegProto(ComboLeg comboLeg, double perLegPrice) {
        ComboLegProto.ComboLeg.Builder comboLegBuilder = ComboLegProto.ComboLeg.newBuilder();
        if (Util.isValidValue(comboLeg.conid())) comboLegBuilder.setConId(comboLeg.conid());
        if (Util.isValidValue(comboLeg.ratio())) comboLegBuilder.setRatio(comboLeg.ratio());
        if (!Util.StringIsEmpty(comboLeg.getAction())) comboLegBuilder.setAction(comboLeg.getAction());
        if (!Util.StringIsEmpty(comboLeg.exchange())) comboLegBuilder.setExchange(comboLeg.exchange());
        if (Util.isValidValue(comboLeg.getOpenClose())) comboLegBuilder.setOpenClose(comboLeg.getOpenClose());
        if (Util.isValidValue(comboLeg.shortSaleSlot())) comboLegBuilder.setShortSalesSlot(comboLeg.shortSaleSlot());
        if (!Util.StringIsEmpty(comboLeg.designatedLocation())) comboLegBuilder.setDesignatedLocation(comboLeg.designatedLocation());
        if (Util.isValidValue(comboLeg.exemptCode())) comboLegBuilder.setExemptCode(comboLeg.exemptCode());
        if (Util.isValidValue(perLegPrice)) comboLegBuilder.setPerLegPrice(perLegPrice);
        return comboLegBuilder.build();
    }

    public static CancelOrderRequestProto.CancelOrderRequest createCancelOrderRequestProto(int id, OrderCancel orderCancel) {
        CancelOrderRequestProto.CancelOrderRequest.Builder cancelOrderRequestBuilder = CancelOrderRequestProto.CancelOrderRequest.newBuilder();
        if (Util.isValidValue(id)) cancelOrderRequestBuilder.setOrderId(id);
        OrderCancelProto.OrderCancel orderCancelproto = createOrderCancelProto(orderCancel);
        if (orderCancelproto != null) cancelOrderRequestBuilder.setOrderCancel(orderCancelproto);
        return cancelOrderRequestBuilder .build();
    }

    public static GlobalCancelRequestProto.GlobalCancelRequest createGlobalCancelRequestProto(OrderCancel orderCancel) {
        GlobalCancelRequestProto.GlobalCancelRequest.Builder globalCancelRequestBuilder = GlobalCancelRequestProto.GlobalCancelRequest.newBuilder();
        OrderCancelProto.OrderCancel orderCancelproto = createOrderCancelProto(orderCancel);
        if (orderCancelproto != null) globalCancelRequestBuilder.setOrderCancel(orderCancelproto);
        return globalCancelRequestBuilder.build();
    }

    public static OrderCancelProto.OrderCancel createOrderCancelProto(OrderCancel orderCancel) {
        OrderCancelProto.OrderCancel.Builder orderCancelBuilder = OrderCancelProto.OrderCancel.newBuilder();
        if (!Util.StringIsEmpty(orderCancel.manualOrderCancelTime())) orderCancelBuilder.setManualOrderCancelTime(orderCancel.manualOrderCancelTime());
        if (!Util.StringIsEmpty(orderCancel.extOperator())) orderCancelBuilder.setExtOperator(orderCancel.extOperator());
        if (Util.isValidValue(orderCancel.manualOrderIndicator())) orderCancelBuilder.setManualOrderIndicator(orderCancel.manualOrderIndicator());
        return orderCancelBuilder.build();
    }
}
