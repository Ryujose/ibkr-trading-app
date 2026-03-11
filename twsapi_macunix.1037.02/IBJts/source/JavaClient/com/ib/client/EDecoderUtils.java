/* Copyright (C) 2025 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

package com.ib.client;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

import com.google.protobuf.InvalidProtocolBufferException;
import com.ib.client.protobuf.ComboLegProto;
import com.ib.client.protobuf.ContractProto;
import com.ib.client.protobuf.DeltaNeutralContractProto;
import com.ib.client.protobuf.ExecutionProto;
import com.ib.client.protobuf.OrderAllocationProto;
import com.ib.client.protobuf.OrderConditionProto;
import com.ib.client.protobuf.OrderProto;
import com.ib.client.protobuf.OrderStateProto;
import com.ib.client.protobuf.SoftDollarTierProto;

public class EDecoderUtils {

    public static Contract decodeContract(ContractProto.Contract contractProto) {
        Contract contract = new Contract();
        if (contractProto.hasConId()) contract.conid(contractProto.getConId());
        if (contractProto.hasSymbol()) contract.symbol(contractProto.getSymbol());
        if (contractProto.hasSecType()) contract.secType(contractProto.getSecType());
        if (contractProto.hasLastTradeDateOrContractMonth()) contract.lastTradeDateOrContractMonth(contractProto.getLastTradeDateOrContractMonth());
        if (contractProto.hasStrike()) contract.strike(contractProto.getStrike());
        if (contractProto.hasRight()) contract.right(contractProto.getRight());
        if (contractProto.hasMultiplier()) contract.multiplier(String.valueOf(contractProto.getMultiplier()));
        if (contractProto.hasExchange()) contract.exchange(contractProto.getExchange());
        if (contractProto.hasCurrency()) contract.currency(contractProto.getCurrency());
        if (contractProto.hasLocalSymbol()) contract.localSymbol(contractProto.getLocalSymbol());
        if (contractProto.hasTradingClass()) contract.tradingClass(contractProto.getTradingClass());
        if (contractProto.hasComboLegsDescrip()) contract.comboLegsDescrip(contractProto.getComboLegsDescrip());
        List<ComboLeg> comboLegs = decodeComboLegs(contractProto); 
        if (comboLegs != null && !comboLegs.isEmpty()) contract.comboLegs(comboLegs);
        DeltaNeutralContract deltaNeutralContract = decodeDeltaNeutralContract(contractProto);
        if (deltaNeutralContract != null) contract.deltaNeutralContract(deltaNeutralContract); 
        return contract;
    }

    public static List<ComboLeg> decodeComboLegs(ContractProto.Contract contractProto) {
        List<ComboLeg> comboLegs = null;

        if (contractProto.getComboLegsCount() > 0) {
            List<ComboLegProto.ComboLeg> comboLegProtoList = contractProto.getComboLegsList();
            comboLegs = new ArrayList<ComboLeg>();

            for (ComboLegProto.ComboLeg comboLegProto : comboLegProtoList) {
                ComboLeg comboLeg = new ComboLeg();
                if (comboLegProto.hasConId()) comboLeg.conid(comboLegProto.getConId());
                if (comboLegProto.hasRatio()) comboLeg.ratio(comboLegProto.getRatio());
                if (comboLegProto.hasAction()) comboLeg.action(comboLegProto.getAction());
                if (comboLegProto.hasExchange()) comboLeg.exchange(comboLegProto.getExchange());
                if (comboLegProto.hasOpenClose()) comboLeg.openClose(comboLegProto.getOpenClose());
                if (comboLegProto.hasShortSalesSlot()) comboLeg.shortSaleSlot(comboLegProto.getShortSalesSlot());
                if (comboLegProto.hasDesignatedLocation()) comboLeg.designatedLocation(comboLegProto.getDesignatedLocation());
                if (comboLegProto.hasExemptCode()) comboLeg.exemptCode(comboLegProto.getExemptCode());
                comboLegs.add(comboLeg);
            }
        }
        return comboLegs;
    }

    public static List<OrderComboLeg> decodeOrderComboLegs(ContractProto.Contract contractProto) {
        List<OrderComboLeg> orderComboLegs = null;
        if (contractProto.getComboLegsCount() > 0) {
            orderComboLegs = new ArrayList<OrderComboLeg>();

            List<ComboLegProto.ComboLeg> comboLegProtoList = contractProto.getComboLegsList();
            for (ComboLegProto.ComboLeg comboLegProto : comboLegProtoList) {
                OrderComboLeg orderComboLeg = comboLegProto.hasPerLegPrice() ? new OrderComboLeg(comboLegProto.getPerLegPrice()) : new OrderComboLeg();
                orderComboLegs.add(orderComboLeg);
            }
        }
        return orderComboLegs;
    }

    public static DeltaNeutralContract decodeDeltaNeutralContract(ContractProto.Contract contractProto) {
        DeltaNeutralContract deltaNeutralContract = null;
        if (contractProto.hasDeltaNeutralContract()) {
            DeltaNeutralContractProto.DeltaNeutralContract deltaNeutralContractProto = contractProto.getDeltaNeutralContract();
            deltaNeutralContract = new DeltaNeutralContract();
            if (deltaNeutralContractProto.hasConId()) deltaNeutralContract.conid(deltaNeutralContractProto.getConId());
            if (deltaNeutralContractProto.hasDelta()) deltaNeutralContract.delta(deltaNeutralContractProto.getDelta());
            if (deltaNeutralContractProto.hasPrice()) deltaNeutralContract.price(deltaNeutralContractProto.getPrice());
        }
        return deltaNeutralContract;
    }

    public static Execution decodeExecution(ExecutionProto.Execution executionProto) {
        Execution execution = new Execution();
        if (executionProto.hasOrderId()) execution.orderId(executionProto.getOrderId());
        if (executionProto.hasClientId()) execution.clientId(executionProto.getClientId());
        if (executionProto.hasExecId()) execution.execId(executionProto.getExecId());
        if (executionProto.hasTime()) execution.time(executionProto.getTime());
        if (executionProto.hasAcctNumber()) execution.acctNumber(executionProto.getAcctNumber());
        if (executionProto.hasExchange()) execution.exchange(executionProto.getExchange());
        if (executionProto.hasSide()) execution.side(executionProto.getSide());
        if (executionProto.hasShares()) execution.shares(Util.stringToDecimal(executionProto.getShares()));
        if (executionProto.hasPrice()) execution.price(executionProto.getPrice());
        if (executionProto.hasPermId()) execution.permId(executionProto.getPermId());
        if (executionProto.hasIsLiquidation()) execution.liquidation(executionProto.getIsLiquidation() ? 1 : 0);
        if (executionProto.hasCumQty()) execution.cumQty(Util.stringToDecimal(executionProto.getCumQty()));
        if (executionProto.hasAvgPrice()) execution.avgPrice(executionProto.getAvgPrice());
        if (executionProto.hasOrderRef()) execution.orderRef(executionProto.getOrderRef());
        if (executionProto.hasEvRule()) execution.evRule(executionProto.getEvRule());
        if (executionProto.hasEvMultiplier()) execution.evMultiplier(executionProto.getEvMultiplier());
        if (executionProto.hasModelCode()) execution.modelCode(executionProto.getModelCode());
        if (executionProto.hasLastLiquidity()) execution.lastLiquidity(executionProto.getLastLiquidity());
        if (executionProto.hasIsPriceRevisionPending()) execution.pendingPriceRevision(executionProto.getIsPriceRevisionPending());
        if (executionProto.hasSubmitter()) execution.submitter(executionProto.getSubmitter());
        if (executionProto.hasOptExerciseOrLapseType()) execution.optExerciseOrLapseType(OptionExerciseType.fromInt(executionProto.getOptExerciseOrLapseType()));
        return execution;
    }
    
    public static Order decodeOrder(ContractProto.Contract contractProto, OrderProto.Order orderProto) throws IOException {
        Order order = new Order();
        if (orderProto.hasAction()) order.action(orderProto.getAction());
        if (orderProto.hasTotalQuantity()) order.totalQuantity(Util.stringToDecimal(orderProto.getTotalQuantity()));
        if (orderProto.hasOrderType()) order.orderType(orderProto.getOrderType());
        if (orderProto.hasLmtPrice()) order.lmtPrice(orderProto.getLmtPrice());
        if (orderProto.hasAuxPrice()) order.auxPrice(orderProto.getAuxPrice());
        if (orderProto.hasTif()) order.tif(orderProto.getTif());
        if (orderProto.hasOcaGroup()) order.ocaGroup(orderProto.getOcaGroup());
        if (orderProto.hasAccount()) order.account(orderProto.getAccount());
        if (orderProto.hasOpenClose()) order.openClose(orderProto.getOpenClose());
        if (orderProto.hasOrigin()) order.origin(orderProto.getOrigin());
        if (orderProto.hasOrderRef()) order.orderRef(orderProto.getOrderRef());
        if (orderProto.hasClientId()) order.clientId(orderProto.getClientId());
        if (orderProto.hasPermId()) order.permId(orderProto.getPermId());
        if (orderProto.hasOutsideRth()) order.outsideRth(orderProto.getOutsideRth());
        if (orderProto.hasHidden()) order.hidden(orderProto.getHidden());
        if (orderProto.hasDiscretionaryAmt()) order.discretionaryAmt(orderProto.getDiscretionaryAmt());
        if (orderProto.hasGoodAfterTime()) order.goodAfterTime(orderProto.getGoodAfterTime());
        if (orderProto.hasFaGroup()) order.faGroup(orderProto.getFaGroup());
        if (orderProto.hasFaMethod()) order.faMethod(orderProto.getFaMethod());
        if (orderProto.hasFaPercentage()) order.faPercentage(orderProto.getFaPercentage());
        if (orderProto.hasModelCode()) order.modelCode(orderProto.getModelCode());
        if (orderProto.hasGoodTillDate()) order.goodTillDate(orderProto.getGoodTillDate());
        if (orderProto.hasRule80A()) order.rule80A(orderProto.getRule80A());
        if (orderProto.hasPercentOffset()) order.percentOffset(orderProto.getPercentOffset());
        if (orderProto.hasSettlingFirm()) order.settlingFirm(orderProto.getSettlingFirm());
        if (orderProto.hasShortSaleSlot()) order.shortSaleSlot(orderProto.getShortSaleSlot());
        if (orderProto.hasDesignatedLocation()) order.designatedLocation(orderProto.getDesignatedLocation());
        if (orderProto.hasExemptCode()) order.exemptCode(orderProto.getExemptCode());
        if (orderProto.hasStartingPrice()) order.startingPrice(orderProto.getStartingPrice());
        if (orderProto.hasStockRefPrice()) order.stockRefPrice(orderProto.getStockRefPrice());
        if (orderProto.hasDelta()) order.delta(orderProto.getDelta());
        if (orderProto.hasStockRangeLower()) order.stockRangeLower(orderProto.getStockRangeLower());
        if (orderProto.hasStockRangeUpper()) order.stockRangeUpper(orderProto.getStockRangeUpper());
        if (orderProto.hasDisplaySize()) order.displaySize(orderProto.getDisplaySize());
        if (orderProto.hasBlockOrder()) order.blockOrder(orderProto.getBlockOrder());
        if (orderProto.hasSweepToFill()) order.sweepToFill(orderProto.getSweepToFill());
        if (orderProto.hasAllOrNone()) order.allOrNone(orderProto.getAllOrNone());
        if (orderProto.hasMinQty()) order.minQty(orderProto.getMinQty());
        if (orderProto.hasOcaType()) order.ocaType(orderProto.getOcaType());
        if (orderProto.hasParentId()) order.parentId(orderProto.getParentId());
        if (orderProto.hasTriggerMethod()) order.triggerMethod(orderProto.getTriggerMethod());
        if (orderProto.hasVolatility()) order.volatility(orderProto.getVolatility());
        if (orderProto.hasVolatilityType()) order.volatilityType(orderProto.getVolatilityType());
        if (orderProto.hasDeltaNeutralOrderType()) order.deltaNeutralOrderType(orderProto.getDeltaNeutralOrderType());
        if (orderProto.hasDeltaNeutralAuxPrice()) order.deltaNeutralAuxPrice(orderProto.getDeltaNeutralAuxPrice());
        if (orderProto.hasDeltaNeutralConId()) order.deltaNeutralConId(orderProto.getDeltaNeutralConId());
        if (orderProto.hasDeltaNeutralSettlingFirm()) order.deltaNeutralSettlingFirm(orderProto.getDeltaNeutralSettlingFirm());
        if (orderProto.hasDeltaNeutralClearingAccount()) order.deltaNeutralClearingAccount(orderProto.getDeltaNeutralClearingAccount());
        if (orderProto.hasDeltaNeutralClearingIntent()) order.deltaNeutralClearingIntent(orderProto.getDeltaNeutralClearingIntent());
        if (orderProto.hasDeltaNeutralOpenClose()) order.deltaNeutralOpenClose(orderProto.getDeltaNeutralOpenClose());
        if (orderProto.hasDeltaNeutralShortSale()) order.deltaNeutralShortSale(orderProto.getDeltaNeutralShortSale());
        if (orderProto.hasDeltaNeutralShortSaleSlot()) order.deltaNeutralShortSaleSlot(orderProto.getDeltaNeutralShortSaleSlot());
        if (orderProto.hasDeltaNeutralDesignatedLocation()) order.deltaNeutralDesignatedLocation(orderProto.getDeltaNeutralDesignatedLocation());
        if (orderProto.hasContinuousUpdate()) order.continuousUpdate(orderProto.getContinuousUpdate() ? 1 : 0);
        if (orderProto.hasReferencePriceType()) order.referencePriceType(orderProto.getReferencePriceType());
        if (orderProto.hasTrailStopPrice()) order.trailStopPrice(orderProto.getTrailStopPrice());
        if (orderProto.hasTrailingPercent()) order.trailingPercent(orderProto.getTrailingPercent());

        List<OrderComboLeg> orderComboLegs = decodeOrderComboLegs(contractProto);
        if (orderComboLegs != null) order.orderComboLegs(orderComboLegs);

        if (orderProto.getSmartComboRoutingParamsCount() > 0) {
            List<TagValue> smartComboRoutingParams = new ArrayList<TagValue>();
            orderProto.getSmartComboRoutingParamsMap().forEach((k, v) -> smartComboRoutingParams.add(new TagValue(k, v)));
            order.smartComboRoutingParams(smartComboRoutingParams);
        }

        if (orderProto.hasScaleInitLevelSize()) order.scaleInitLevelSize(orderProto.getScaleInitLevelSize());
        if (orderProto.hasScaleSubsLevelSize()) order.scaleSubsLevelSize(orderProto.getScaleSubsLevelSize());
        if (orderProto.hasScalePriceIncrement()) order.scalePriceIncrement(orderProto.getScalePriceIncrement());
        if (orderProto.hasScalePriceAdjustValue()) order.scalePriceAdjustValue(orderProto.getScalePriceAdjustValue());
        if (orderProto.hasScalePriceAdjustInterval()) order.scalePriceAdjustInterval(orderProto.getScalePriceAdjustInterval());
        if (orderProto.hasScaleProfitOffset()) order.scaleProfitOffset(orderProto.getScaleProfitOffset());
        if (orderProto.hasScaleAutoReset()) order.scaleAutoReset(orderProto.getScaleAutoReset());
        if (orderProto.hasScaleInitPosition()) order.scaleInitPosition(orderProto.getScaleInitPosition());
        if (orderProto.hasScaleInitFillQty()) order.scaleInitFillQty(orderProto.getScaleInitFillQty());
        if (orderProto.hasScaleRandomPercent()) order.scaleRandomPercent(orderProto.getScaleRandomPercent());
        if (orderProto.hasHedgeType()) order.hedgeType(orderProto.getHedgeType());
        if (orderProto.hasHedgeType() && orderProto.hasHedgeParam() && !Util.StringIsEmpty(orderProto.getHedgeType())) order.hedgeParam(orderProto.getHedgeParam());
        if (orderProto.hasOptOutSmartRouting()) order.optOutSmartRouting(orderProto.getOptOutSmartRouting());
        if (orderProto.hasClearingAccount()) order.clearingAccount(orderProto.getClearingAccount());
        if (orderProto.hasClearingIntent()) order.clearingIntent(orderProto.getClearingIntent());
        if (orderProto.hasNotHeld()) order.notHeld(orderProto.getNotHeld());

        if (orderProto.hasAlgoStrategy()) {
            order.algoStrategy(orderProto.getAlgoStrategy());
            if (orderProto.getAlgoParamsCount() > 0) {
                List<TagValue> algoParams = new ArrayList<TagValue>();
                orderProto.getAlgoParamsMap().forEach((k, v) -> algoParams.add(new TagValue(k, v)));
                order.algoParams(algoParams);
            }
        }

        if (orderProto.hasSolicited()) order.solicited(orderProto.getSolicited());
        if (orderProto.hasWhatIf()) order.whatIf(orderProto.getWhatIf());
        if (orderProto.hasRandomizeSize()) order.randomizeSize(orderProto.getRandomizeSize());
        if (orderProto.hasRandomizePrice()) order.randomizePrice(orderProto.getRandomizePrice());
        if (orderProto.hasReferenceContractId()) order.referenceContractId(orderProto.getReferenceContractId());
        if (orderProto.hasIsPeggedChangeAmountDecrease()) order.isPeggedChangeAmountDecrease(orderProto.getIsPeggedChangeAmountDecrease());
        if (orderProto.hasPeggedChangeAmount()) order.peggedChangeAmount(orderProto.getPeggedChangeAmount());
        if (orderProto.hasReferenceChangeAmount()) order.referenceChangeAmount(orderProto.getReferenceChangeAmount());
        if (orderProto.hasReferenceExchangeId()) order.referenceExchangeId(orderProto.getReferenceExchangeId());

        List<OrderCondition> conditions = decodeConditions(orderProto);
        if (conditions != null) order.conditions(conditions);
        if (orderProto.hasConditionsIgnoreRth()) order.conditionsIgnoreRth(orderProto.getConditionsIgnoreRth());
        if (orderProto.hasConditionsCancelOrder()) order.conditionsCancelOrder(orderProto.getConditionsCancelOrder());

        if (orderProto.hasAdjustedOrderType()) order.adjustedOrderType(OrderType.get(orderProto.getAdjustedOrderType()));
        if (orderProto.hasTriggerPrice()) order.triggerPrice(orderProto.getTriggerPrice());
        if (orderProto.hasLmtPriceOffset()) order.lmtPriceOffset(orderProto.getLmtPriceOffset());
        if (orderProto.hasAdjustedStopPrice()) order.adjustedStopPrice(orderProto.getAdjustedStopPrice());
        if (orderProto.hasAdjustedStopLimitPrice()) order.adjustedStopLimitPrice(orderProto.getAdjustedStopLimitPrice());
        if (orderProto.hasAdjustedTrailingAmount()) order.adjustedTrailingAmount(orderProto.getAdjustedTrailingAmount());
        if (orderProto.hasAdjustableTrailingUnit()) order.adjustableTrailingUnit(orderProto.getAdjustableTrailingUnit());

        SoftDollarTier softDollarTier = decodeSoftDollarTier(orderProto);
        if (softDollarTier != null) order.softDollarTier(softDollarTier);

        if (orderProto.hasCashQty()) order.cashQty(orderProto.getCashQty());
        if (orderProto.hasDontUseAutoPriceForHedge()) order.dontUseAutoPriceForHedge(orderProto.getDontUseAutoPriceForHedge());
        if (orderProto.hasIsOmsContainer()) order.isOmsContainer(orderProto.getIsOmsContainer());
        if (orderProto.hasDiscretionaryUpToLimitPrice()) order.discretionaryUpToLimitPrice(orderProto.getDiscretionaryUpToLimitPrice());
        order.usePriceMgmtAlgo(orderProto.getUsePriceMgmtAlgo() != 0);
        if (orderProto.hasDuration()) order.duration(orderProto.getDuration());
        if (orderProto.hasPostToAts()) order.postToAts(orderProto.getPostToAts());
        if (orderProto.hasAutoCancelParent()) order.autoCancelParent(orderProto.getAutoCancelParent());
        if (orderProto.hasMinTradeQty()) order.minTradeQty(orderProto.getMinTradeQty());
        if (orderProto.hasMinCompeteSize()) order.minCompeteSize(orderProto.getMinCompeteSize());
        if (orderProto.hasCompeteAgainstBestOffset()) order.competeAgainstBestOffset(orderProto.getCompeteAgainstBestOffset());
        if (orderProto.hasMidOffsetAtWhole()) order.midOffsetAtWhole(orderProto.getMidOffsetAtWhole());
        if (orderProto.hasMidOffsetAtHalf()) order.midOffsetAtHalf(orderProto.getMidOffsetAtHalf());
        if (orderProto.hasCustomerAccount()) order.customerAccount(orderProto.getCustomerAccount());
        if (orderProto.hasProfessionalCustomer()) order.professionalCustomer(orderProto.getProfessionalCustomer());
        if (orderProto.hasBondAccruedInterest()) order.bondAccruedInterest(orderProto.getBondAccruedInterest());
        if (orderProto.hasIncludeOvernight()) order.includeOvernight(orderProto.getIncludeOvernight());
        if (orderProto.hasExtOperator()) order.extOperator(orderProto.getExtOperator());
        if (orderProto.hasManualOrderIndicator()) order.manualOrderIndicator(orderProto.getManualOrderIndicator());
        if (orderProto.hasSubmitter()) order.submitter(orderProto.getSubmitter());
        if (orderProto.hasImbalanceOnly()) order.imbalanceOnly(orderProto.getImbalanceOnly());

        return order;
    }

    public static List<OrderCondition> decodeConditions(OrderProto.Order order) throws IOException {
        List<OrderCondition> orderConditions = null;

        try {
            if (order.getConditionsCount() > 0) {
                orderConditions = new ArrayList<OrderCondition>();
                for (OrderConditionProto.OrderCondition orderConditionProto : order.getConditionsList()) {
                    int conditionTypeValue = orderConditionProto.hasType() ? orderConditionProto.getType() : 0;
                    OrderConditionType conditionType = OrderConditionType.fromInt(conditionTypeValue);

                    OrderCondition orderCondition = null;
                    switch(conditionType) {
                        case Price:
                            orderCondition = createPriceCondition(orderConditionProto);
                            break;
                        case Time:
                            orderCondition = createTimeCondition(orderConditionProto);
                            break;
                        case Margin:
                            orderCondition = createMarginCondition(orderConditionProto);
                            break;
                        case Execution:
                            orderCondition = createExecutionCondition(orderConditionProto);
                            break;
                        case Volume:
                            orderCondition = createVolumeCondition(orderConditionProto);
                            break;
                        case PercentChange:
                            orderCondition = createPercentChangeCondition(orderConditionProto);
                            break;
                    }
                    if (orderCondition != null) {
                        orderConditions.add(orderCondition);
                    }
                }
            }
        } catch (InvalidProtocolBufferException e) {
            throw new IOException("Error decoding conditions", e);
        }

        return orderConditions;
    }

    private static void setConditionFields(OrderConditionProto.OrderCondition orderConditionProto, OrderCondition orderCondition) {
        if (orderConditionProto.hasIsConjunctionConnection()) orderCondition.conjunctionConnection(orderConditionProto.getIsConjunctionConnection());
    }

    private static void setOperatorConditionFields(OrderConditionProto.OrderCondition orderConditionProto, OperatorCondition operatorCondition) throws InvalidProtocolBufferException {
        setConditionFields(orderConditionProto, operatorCondition);
        if (orderConditionProto.hasIsMore()) operatorCondition.isMore(orderConditionProto.getIsMore());
    }

    private static void setContractConditionFields(OrderConditionProto.OrderCondition orderConditionProto, ContractCondition contractCondition) throws InvalidProtocolBufferException {
        setOperatorConditionFields(orderConditionProto, contractCondition);
        if (orderConditionProto.hasConId()) contractCondition.conId(orderConditionProto.getConId());
        if (orderConditionProto.hasExchange()) contractCondition.exchange(orderConditionProto.getExchange());
    }

    private static PriceCondition createPriceCondition(OrderConditionProto.OrderCondition orderConditionProto) throws InvalidProtocolBufferException {
        PriceCondition priceCondition = (PriceCondition)OrderCondition.create(OrderConditionType.Price);
        setContractConditionFields(orderConditionProto, priceCondition);
        if (orderConditionProto.hasPrice()) priceCondition.price(orderConditionProto.getPrice());
        if (orderConditionProto.hasTriggerMethod()) priceCondition.triggerMethod(orderConditionProto.getTriggerMethod());
        return priceCondition;
    }

    private static TimeCondition createTimeCondition(OrderConditionProto.OrderCondition orderConditionProto) throws InvalidProtocolBufferException {
        TimeCondition timeCondition = (TimeCondition)OrderCondition.create(OrderConditionType.Time);
        setOperatorConditionFields(orderConditionProto, timeCondition);
        if (orderConditionProto.hasTime()) timeCondition.time(orderConditionProto.getTime());
        return timeCondition;
    }

    private static MarginCondition createMarginCondition(OrderConditionProto.OrderCondition orderConditionProto) throws InvalidProtocolBufferException {
        MarginCondition marginCondition = (MarginCondition)OrderCondition.create(OrderConditionType.Margin);
        setOperatorConditionFields(orderConditionProto, marginCondition);
        if (orderConditionProto.hasPercent()) marginCondition.percent(orderConditionProto.getPercent());
        return marginCondition;
    }

    private static ExecutionCondition createExecutionCondition(OrderConditionProto.OrderCondition orderConditionProto) throws InvalidProtocolBufferException {
        ExecutionCondition executionCondition = (ExecutionCondition)OrderCondition.create(OrderConditionType.Execution);
        setConditionFields(orderConditionProto, executionCondition);
        if (orderConditionProto.hasSecType()) executionCondition.secType(orderConditionProto.getSecType());
        if (orderConditionProto.hasExchange()) executionCondition.exchange(orderConditionProto.getExchange());
        if (orderConditionProto.hasSymbol()) executionCondition.symbol(orderConditionProto.getSymbol());
        return executionCondition;
    }

    private static VolumeCondition createVolumeCondition(OrderConditionProto.OrderCondition orderConditionProto) throws InvalidProtocolBufferException {
        VolumeCondition volumeCondition = (VolumeCondition)OrderCondition.create(OrderConditionType.Volume);
        setContractConditionFields(orderConditionProto, volumeCondition);
        if (orderConditionProto.hasVolume()) volumeCondition.volume(orderConditionProto.getVolume());
        return volumeCondition;
    }

    private static PercentChangeCondition createPercentChangeCondition(OrderConditionProto.OrderCondition orderConditionProto) throws InvalidProtocolBufferException {
        PercentChangeCondition percentChangeCondition = (PercentChangeCondition)OrderCondition.create(OrderConditionType.PercentChange);
        setContractConditionFields(orderConditionProto, percentChangeCondition);
        if (orderConditionProto.hasChangePercent()) percentChangeCondition.changePercent(orderConditionProto.getChangePercent());
        return percentChangeCondition;
    }

    public static SoftDollarTier decodeSoftDollarTier(OrderProto.Order order) {
        SoftDollarTier softDollarTier = null;
        SoftDollarTierProto.SoftDollarTier softDollarTierProto = order.hasSoftDollarTier() ? order.getSoftDollarTier() : null;
        if (softDollarTierProto != null) {
            String name = softDollarTierProto.hasName() ? softDollarTierProto.getName() : null;
            String value = softDollarTierProto.hasValue() ? softDollarTierProto.getValue() : null;
            String displayName = softDollarTierProto.hasDisplayName() ? softDollarTierProto.getDisplayName() : null;
            softDollarTier = new SoftDollarTier(name, value, displayName);
        }
        return softDollarTier;
    }

    public static OrderState decodeOrderState(OrderStateProto.OrderState orderStateProto) {
        OrderState orderState = new OrderState();
        if (orderStateProto.hasStatus()) orderState.status(orderStateProto.getStatus());
        if (orderStateProto.hasInitMarginBefore()) orderState.initMarginBefore(String.valueOf(orderStateProto.getInitMarginBefore()));
        if (orderStateProto.hasMaintMarginBefore()) orderState.maintMarginBefore(String.valueOf(orderStateProto.getMaintMarginBefore()));
        if (orderStateProto.hasEquityWithLoanBefore()) orderState.equityWithLoanBefore(String.valueOf(orderStateProto.getEquityWithLoanBefore()));
        if (orderStateProto.hasInitMarginChange()) orderState.initMarginChange(String.valueOf(orderStateProto.getInitMarginChange()));
        if (orderStateProto.hasMaintMarginChange()) orderState.maintMarginChange(String.valueOf(orderStateProto.getMaintMarginChange()));
        if (orderStateProto.hasEquityWithLoanChange()) orderState.equityWithLoanChange(String.valueOf(orderStateProto.getEquityWithLoanChange()));
        if (orderStateProto.hasInitMarginAfter()) orderState.initMarginAfter(String.valueOf(orderStateProto.getInitMarginAfter()));
        if (orderStateProto.hasMaintMarginAfter()) orderState.maintMarginAfter(String.valueOf(orderStateProto.getMaintMarginAfter()));
        if (orderStateProto.hasEquityWithLoanAfter()) orderState.equityWithLoanAfter(String.valueOf(orderStateProto.getEquityWithLoanAfter()));
        if (orderStateProto.hasCommissionAndFees()) orderState.commissionAndFees(orderStateProto.getCommissionAndFees());
        if (orderStateProto.hasMinCommissionAndFees()) orderState.minCommissionAndFees(orderStateProto.getMinCommissionAndFees());
        if (orderStateProto.hasMaxCommissionAndFees()) orderState.maxCommissionAndFees(orderStateProto.getMaxCommissionAndFees());
        if (orderStateProto.hasCommissionAndFeesCurrency()) orderState.commissionAndFeesCurrency(orderStateProto.getCommissionAndFeesCurrency());
        if (orderStateProto.hasWarningText()) orderState.warningText(orderStateProto.getWarningText());
        if (orderStateProto.hasMarginCurrency()) orderState.marginCurrency(orderStateProto.getMarginCurrency());
        if (orderStateProto.hasInitMarginBeforeOutsideRTH()) orderState.initMarginBeforeOutsideRTH(orderStateProto.getInitMarginBeforeOutsideRTH());
        if (orderStateProto.hasMaintMarginBeforeOutsideRTH()) orderState.maintMarginBeforeOutsideRTH(orderStateProto.getMaintMarginBeforeOutsideRTH());
        if (orderStateProto.hasEquityWithLoanBeforeOutsideRTH()) orderState.equityWithLoanBeforeOutsideRTH(orderStateProto.getEquityWithLoanBeforeOutsideRTH());
        if (orderStateProto.hasInitMarginChangeOutsideRTH()) orderState.initMarginChangeOutsideRTH(orderStateProto.getInitMarginChangeOutsideRTH());
        if (orderStateProto.hasMaintMarginChangeOutsideRTH()) orderState.maintMarginChangeOutsideRTH(orderStateProto.getMaintMarginChangeOutsideRTH());
        if (orderStateProto.hasEquityWithLoanChangeOutsideRTH()) orderState.equityWithLoanChangeOutsideRTH(orderStateProto.getEquityWithLoanChangeOutsideRTH());
        if (orderStateProto.hasInitMarginAfterOutsideRTH()) orderState.initMarginAfterOutsideRTH(orderStateProto.getInitMarginAfterOutsideRTH());
        if (orderStateProto.hasMaintMarginAfterOutsideRTH()) orderState.maintMarginAfterOutsideRTH(orderStateProto.getMaintMarginAfterOutsideRTH());
        if (orderStateProto.hasEquityWithLoanAfterOutsideRTH()) orderState.equityWithLoanAfterOutsideRTH(orderStateProto.getEquityWithLoanAfterOutsideRTH());
        if (orderStateProto.hasSuggestedSize()) orderState.suggestedSize(Util.stringToDecimal(orderStateProto.getSuggestedSize()));
        if (orderStateProto.hasRejectReason()) orderState.rejectReason(orderStateProto.getRejectReason());

        List<OrderAllocation> orderAllocations = decodeOrderAllocations(orderStateProto);
        if (orderAllocations != null) orderState.orderAllocations(orderAllocations);

        return orderState;
    }

    public static List<OrderAllocation> decodeOrderAllocations(OrderStateProto.OrderState orderStateProto) {
        List<OrderAllocation> orderAllocations = null;
        if (orderStateProto.getOrderAllocationsCount() > 0) {
            orderAllocations = new ArrayList<OrderAllocation>();
            for (OrderAllocationProto.OrderAllocation orderAllocationProto : orderStateProto.getOrderAllocationsList()) {
                OrderAllocation orderAllocation = new OrderAllocation();
                if (orderAllocationProto.hasAccount()) orderAllocation.account(orderAllocationProto.getAccount());
                if (orderAllocationProto.hasPosition()) orderAllocation.position(Util.stringToDecimal(orderAllocationProto.getPosition()));
                if (orderAllocationProto.hasPositionDesired()) orderAllocation.positionDesired(Util.stringToDecimal(orderAllocationProto.getPositionDesired()));
                if (orderAllocationProto.hasPositionAfter()) orderAllocation.positionAfter(Util.stringToDecimal(orderAllocationProto.getPositionAfter()));
                if (orderAllocationProto.hasDesiredAllocQty()) orderAllocation.desiredAllocQty(Util.stringToDecimal(orderAllocationProto.getDesiredAllocQty()));
                if (orderAllocationProto.hasAllowedAllocQty()) orderAllocation.allowedAllocQty(Util.stringToDecimal(orderAllocationProto.getAllowedAllocQty()));
                if (orderAllocationProto.hasIsMonetary()) orderAllocation.isMonetary(orderAllocationProto.getIsMonetary());
                orderAllocations.add(orderAllocation);
            }
        }
        return orderAllocations;
    }
}
