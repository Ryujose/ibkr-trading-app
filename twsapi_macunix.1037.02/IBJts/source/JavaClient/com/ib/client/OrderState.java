/* Copyright (C) 2024 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

package com.ib.client;

import java.util.ArrayList;
import java.util.List;

public class OrderState {
    private String m_status;
    private String m_initMarginBefore;
    private String m_maintMarginBefore;
    private String m_equityWithLoanBefore;
    private String m_initMarginChange;
    private String m_maintMarginChange;
    private String m_equityWithLoanChange;
    private String m_initMarginAfter;
    private String m_maintMarginAfter;
    private String m_equityWithLoanAfter;
    private double m_commissionAndFees;
    private double m_minCommissionAndFees;
    private double m_maxCommissionAndFees;
    private String m_commissionAndFeesCurrency;
    private String m_marginCurrency;
    private double m_initMarginBeforeOutsideRTH;
    private double m_maintMarginBeforeOutsideRTH;
    private double m_equityWithLoanBeforeOutsideRTH;
    private double m_initMarginChangeOutsideRTH;
    private double m_maintMarginChangeOutsideRTH;
    private double m_equityWithLoanChangeOutsideRTH;
    private double m_initMarginAfterOutsideRTH;
    private double m_maintMarginAfterOutsideRTH;
    private double m_equityWithLoanAfterOutsideRTH;
    private Decimal m_suggestedSize;
    private String m_rejectReason;
    private List<OrderAllocation> m_orderAllocations;
    private String m_warningText;
    private String m_completedTime;
    private String m_completedStatus;

    // Get
    public double commissionAndFees()    { return m_commissionAndFees; }
    public double maxCommissionAndFees() { return m_maxCommissionAndFees; }
    public double minCommissionAndFees() { return m_minCommissionAndFees; }
    public OrderStatus status()          { return OrderStatus.get(m_status); }
    public String getStatus()            { return m_status; }
    public String commissionAndFeesCurrency() { return m_commissionAndFeesCurrency; }
    public String initMarginBefore()     { return m_initMarginBefore; }
    public String maintMarginBefore()    { return m_maintMarginBefore; }
    public String equityWithLoanBefore() { return m_equityWithLoanBefore; }
    public String initMarginChange()     { return m_initMarginChange; }
    public String maintMarginChange()    { return m_maintMarginChange; }
    public String equityWithLoanChange() { return m_equityWithLoanChange; }
    public String initMarginAfter()      { return m_initMarginAfter; }
    public String maintMarginAfter()     { return m_maintMarginAfter; }
    public String equityWithLoanAfter()  { return m_equityWithLoanAfter; }
    public String marginCurrency()                  { return m_marginCurrency; }
    public double initMarginBeforeOutsideRTH()      { return m_initMarginBeforeOutsideRTH; }
    public double maintMarginBeforeOutsideRTH()     { return m_maintMarginBeforeOutsideRTH; }
    public double equityWithLoanBeforeOutsideRTH()  { return m_equityWithLoanBeforeOutsideRTH; }
    public double initMarginChangeOutsideRTH()      { return m_initMarginChangeOutsideRTH; }
    public double maintMarginChangeOutsideRTH()     { return m_maintMarginChangeOutsideRTH; }
    public double equityWithLoanChangeOutsideRTH()  { return m_equityWithLoanChangeOutsideRTH; }
    public double initMarginAfterOutsideRTH()       { return m_initMarginAfterOutsideRTH; }
    public double maintMarginAfterOutsideRTH()      { return m_maintMarginAfterOutsideRTH; }
    public double equityWithLoanAfterOutsideRTH()   { return m_equityWithLoanAfterOutsideRTH; }
    public Decimal suggestedSize()                  { return m_suggestedSize; }
    public String rejectReason()                    { return m_rejectReason; }
    public List<OrderAllocation> orderAllocations() { return m_orderAllocations; }
    public String warningText()          { return m_warningText; }
    public String completedTime()        { return m_completedTime; }
    public String completedStatus()      { return m_completedStatus; }

    // Set
    public void commissionAndFees(double v)    { m_commissionAndFees = v; }
    public void commissionAndFeesCurrency(String v) { m_commissionAndFeesCurrency = v; }
    public void initMarginBefore(String v)     { m_initMarginBefore = v; }
    public void maintMarginBefore(String v)    { m_maintMarginBefore = v; }
    public void equityWithLoanBefore(String v) { m_equityWithLoanBefore = v; }
    public void initMarginChange(String v)     { m_initMarginChange = v; }
    public void maintMarginChange(String v)    { m_maintMarginChange = v; }
    public void equityWithLoanChange(String v) { m_equityWithLoanChange = v; }
    public void initMarginAfter(String v)      { m_initMarginAfter = v; }
    public void maintMarginAfter(String v)     { m_maintMarginAfter = v; }
    public void equityWithLoanAfter(String v)  { m_equityWithLoanAfter = v; }
    public void maxCommissionAndFees(double v) { m_maxCommissionAndFees = v; }
    public void minCommissionAndFees(double v) { m_minCommissionAndFees = v; }
    public void status(OrderStatus v)          { m_status = ( v == null ) ? null : v.name(); }
    public void status(String v)               { m_status = v; }
    public void marginCurrency(String v)                  { m_marginCurrency = v; }
    public void initMarginBeforeOutsideRTH(double v)      { m_initMarginBeforeOutsideRTH = v; }
    public void maintMarginBeforeOutsideRTH(double v)     { m_maintMarginBeforeOutsideRTH = v; }
    public void equityWithLoanBeforeOutsideRTH(double v)  { m_equityWithLoanBeforeOutsideRTH = v; }
    public void initMarginChangeOutsideRTH(double v)      { m_initMarginChangeOutsideRTH = v; }
    public void maintMarginChangeOutsideRTH(double v)     { m_maintMarginChangeOutsideRTH = v; }
    public void equityWithLoanChangeOutsideRTH(double v)  { m_equityWithLoanChangeOutsideRTH = v; }
    public void initMarginAfterOutsideRTH(double v)       { m_initMarginAfterOutsideRTH = v; }
    public void maintMarginAfterOutsideRTH(double v)      { m_maintMarginAfterOutsideRTH = v; }
    public void equityWithLoanAfterOutsideRTH(double v)   { m_equityWithLoanAfterOutsideRTH = v; }
    public void suggestedSize(Decimal v)                  { m_suggestedSize = v; }
    public void rejectReason(String v)                    { m_rejectReason = v; }
    public void orderAllocations(List<OrderAllocation> v) { m_orderAllocations = v; }
    public void warningText(String v)          { m_warningText = v; }
    public void completedTime(String v)        { m_completedTime = v; }
    public void completedStatus(String v)      { m_completedStatus = v; }
    
    OrderState() {
        m_status = "";
        m_initMarginBefore = "";
        m_maintMarginBefore = "";
        m_equityWithLoanBefore = "";
        m_initMarginChange = "";
        m_maintMarginChange = "";
        m_equityWithLoanChange = "";
        m_initMarginAfter = "";
        m_maintMarginAfter = "";
        m_equityWithLoanAfter = "";
        m_commissionAndFees = Double.MAX_VALUE;
        m_minCommissionAndFees = Double.MAX_VALUE;
        m_maxCommissionAndFees = Double.MAX_VALUE;
        m_commissionAndFeesCurrency = "";
        m_marginCurrency = "";
        m_initMarginBeforeOutsideRTH = Double.MAX_VALUE;
        m_maintMarginBeforeOutsideRTH = Double.MAX_VALUE;
        m_equityWithLoanBeforeOutsideRTH = Double.MAX_VALUE;
        m_initMarginChangeOutsideRTH = Double.MAX_VALUE;
        m_maintMarginChangeOutsideRTH = Double.MAX_VALUE;
        m_equityWithLoanChangeOutsideRTH = Double.MAX_VALUE;
        m_initMarginAfterOutsideRTH = Double.MAX_VALUE;
        m_maintMarginAfterOutsideRTH = Double.MAX_VALUE;
        m_equityWithLoanAfterOutsideRTH = Double.MAX_VALUE;
        m_suggestedSize = Decimal.INVALID;
        m_rejectReason = "";
        m_orderAllocations = new ArrayList<>();
        m_warningText = "";
        m_completedTime = "";
        m_completedStatus = "";
    }

	@Override
    public boolean equals(Object other) {
        if (this == other) {
            return true;
        }
        if (!(other instanceof OrderState)) {
            return false;
        }
        OrderState state = (OrderState)other;

        if (m_commissionAndFees != state.m_commissionAndFees ||
            m_minCommissionAndFees != state.m_minCommissionAndFees ||
            m_maxCommissionAndFees != state.m_maxCommissionAndFees ||
            m_initMarginBeforeOutsideRTH != state.m_initMarginBeforeOutsideRTH ||
            m_maintMarginBeforeOutsideRTH != state.m_maintMarginBeforeOutsideRTH ||
            m_equityWithLoanBeforeOutsideRTH != state.m_equityWithLoanBeforeOutsideRTH ||
            m_initMarginChangeOutsideRTH != state.m_initMarginChangeOutsideRTH ||
            m_maintMarginChangeOutsideRTH != state.m_maintMarginChangeOutsideRTH ||
            m_equityWithLoanChangeOutsideRTH != state.m_equityWithLoanChangeOutsideRTH ||
            m_initMarginAfterOutsideRTH != state.m_initMarginAfterOutsideRTH ||
            m_maintMarginAfterOutsideRTH != state.m_maintMarginAfterOutsideRTH ||
            m_equityWithLoanAfterOutsideRTH != state.m_equityWithLoanAfterOutsideRTH ||
            m_suggestedSize != state.m_suggestedSize) {
            return false;
        }

        if (Util.StringCompare(m_status, state.m_status) != 0 ||
            Util.StringCompare(m_initMarginBefore, state.m_initMarginBefore) != 0 ||
            Util.StringCompare(m_maintMarginBefore, state.m_maintMarginBefore) != 0 ||
            Util.StringCompare(m_equityWithLoanBefore, state.m_equityWithLoanBefore) != 0 ||
            Util.StringCompare(m_initMarginChange, state.m_initMarginChange) != 0 ||
            Util.StringCompare(m_maintMarginChange, state.m_maintMarginChange) != 0 ||
            Util.StringCompare(m_equityWithLoanChange, state.m_equityWithLoanChange) != 0 ||
            Util.StringCompare(m_initMarginAfter, state.m_initMarginAfter) != 0 ||
            Util.StringCompare(m_maintMarginAfter, state.m_maintMarginAfter) != 0 ||
            Util.StringCompare(m_equityWithLoanAfter, state.m_equityWithLoanAfter) != 0 ||
            Util.StringCompare(m_commissionAndFeesCurrency, state.m_commissionAndFeesCurrency) != 0 ||
            Util.StringCompare(m_marginCurrency, state.m_marginCurrency) != 0 ||
            Util.StringCompare(m_rejectReason, state.m_rejectReason) != 0 ||
            Util.StringCompare(m_completedTime, state.m_completedTime) != 0) {
            return false;
        }

        if (!Util.listsEqualUnordered(m_orderAllocations, state.m_orderAllocations)) {
            return false;
        }
        return true;
    }

    @Override
    public int hashCode() {
        // Use a few fields as a compromise between performance and hashCode quality.
        int result;
        long temp;
        temp = Double.doubleToLongBits(m_commissionAndFees);
        result = (int) (temp ^ (temp >>> 32));
        temp = Double.doubleToLongBits(m_minCommissionAndFees);
        result = 31 * result + (int) (temp ^ (temp >>> 32));
        temp = Double.doubleToLongBits(m_maxCommissionAndFees);
        result = 31 * result + (int) (temp ^ (temp >>> 32));
        return result;
    }
}
