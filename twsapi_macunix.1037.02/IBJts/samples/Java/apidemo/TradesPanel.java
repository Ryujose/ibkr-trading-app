/* Copyright (C) 2025 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

package apidemo;

import java.awt.BorderLayout;
import java.awt.FlowLayout;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JTable;
import javax.swing.border.TitledBorder;
import javax.swing.table.AbstractTableModel;

import com.ib.client.CommissionAndFeesReport;
import com.ib.client.Contract;
import com.ib.client.Execution;
import com.ib.client.ExecutionFilter;
import com.ib.client.Util;
import com.ib.controller.ApiController.ITradeReportHandler;

import apidemo.util.HtmlButton;

public class TradesPanel extends JPanel implements ITradeReportHandler {
	private List<FullExec> m_trades = new ArrayList<>();
	private Map<String,FullExec> m_map = new HashMap<>();
	private Model m_model = new Model();
	
	TradesPanel() {
		JTable table = new JTable( m_model);
		JScrollPane scroll = new JScrollPane( table);
		scroll.setBorder( new TitledBorder( "Trade Log"));
		
		HtmlButton but = new HtmlButton( "Refresh") {
			@Override public void actionPerformed() {
				onRefresh();
			}
		};

		JPanel p = new JPanel( new FlowLayout( FlowLayout.RIGHT));
		p.add( but);
		
		setLayout( new BorderLayout() );
		add( scroll);
		add( p, BorderLayout.SOUTH);
	}

	public void activated() {
		onRefresh();
	}
	
	private void onRefresh() {
		ExecutionFilter executionFilter = new ExecutionFilter();
		executionFilter.lastNDays(7);
		ApiDemo.INSTANCE.controller().reqExecutions(executionFilter, this);
	}

	@Override public void tradeReport(String tradeKey, Contract contract, Execution trade) {
		FullExec full = m_map.get( tradeKey);
		
		if (full != null) {
			full.m_trade = trade;
		}
		else {
			full = new FullExec( contract, trade);
			m_trades.add( full);
			m_map.put( tradeKey, full);
		}
		
		m_model.fireTableDataChanged();
	}
	
	@Override public void tradeReportEnd() {
	}
	
	@Override public void commissionAndFeesReport(String tradeKey, CommissionAndFeesReport commissionAndFeesReport) {
		FullExec full = m_map.get( tradeKey);
		if (full != null) {
			full.m_commissionAndFeesReport = commissionAndFeesReport;
		}
	}

	private class Model extends AbstractTableModel {
		@Override public int getRowCount() {
			return m_trades.size();
		}

		@Override public int getColumnCount() {
			return 13;
		}
		
		@Override public String getColumnName(int col) {
			switch( col) {
				case 0: return "Date/Time";
				case 1: return "Account";
				case 2: return "Model Code";
				case 3: return "Action";
				case 4: return "Quantity";
				case 5: return "Description";
				case 6: return "Price";
				case 7: return "Commission and Fees";
				case 8: return "Last Liquidity";
				case 9: return "Pending Price Revision";
				case 10: return "PermId";
				case 11: return "Submitter";
				case 12: return "OptExerciseOrLapseType";
				default: return null;
			}
		}

		@Override public Object getValueAt(int row, int col) {
			FullExec full = m_trades.get( row);
			
			switch( col) {
				case 0: return full.m_trade.time();
				case 1: return full.m_trade.acctNumber();
				case 2: return full.m_trade.modelCode();
				case 3: return full.m_trade.side();
				case 4: return full.m_trade.shares();
				case 5: return full.m_contract.textDescription();
				case 6: return Util.DoubleMaxString(full.m_trade.price());
				case 7: return full.m_commissionAndFeesReport != null ? Util.DoubleMaxString(full.m_commissionAndFeesReport.commissionAndFees()) : null;
				case 8: return full.m_trade.lastLiquidity();
				case 9: return full.m_trade.pendingPriceRevision();
				case 10: return Util.LongMaxString(full.m_trade.permId());
				case 11: return full.m_trade.submitter();
				case 12: return full.m_trade.optExerciseOrLapseType();
				default: return null;
			}
		}
	}

	static class FullExec {
		Contract m_contract;
		Execution m_trade;
		CommissionAndFeesReport m_commissionAndFeesReport;
		
		FullExec( Contract contract, Execution trade) {
			m_contract = contract;
			m_trade = trade;
		}
	}
}
