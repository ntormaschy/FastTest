import pandas as pd 
import numpy as np

from Strategy import Strategy
from Exchange import Exchange
from Broker import Broker
from Asset import Asset
from Order import *

class FastTest():
    def __init__(self, exchange : Exchange, broker : Broker, **kwargs) -> None:
        self.exchange = exchange
        self.broker = broker
        self.strategy = kwargs.get("strategy")
        self.exchange.build()
        self.portfolio_value_history = []

    def register_strategy(self, strategy : Strategy):
        self.strategy = strategy

    def analyze_strategy_on_next(self):
        self.portfolio_value_history.append(self.broker.net_liquidation_value)

    def analyze_strategy_on_finish(self):
        for asset_analysis in self.broker.strategy_analysis.values():
            number_trades = (asset_analysis["wins"] + asset_analysis["losses"])
            if number_trades == 0: continue
            asset_analysis["number_trades"] = number_trades
            asset_analysis["win_rate"] = asset_analysis["wins"] / number_trades
            asset_analysis["average_time_in_market"] = asset_analysis["time_in_market"] / number_trades
            asset_analysis["average_position_size"] = asset_analysis["total_units"] / number_trades
            asset_analysis["average_pl"] = asset_analysis["pl"] / number_trades

    def run(self):
        
        #intilze broker
        self.broker.build()

        while self.exchange.next():

            #evaluate collateral held by broker
            self.broker.evaluate_collateral()

            #allow exchange to process open orders from previous steps
            filled_orders = self.exchange.process_orders()

            #allow broker to process orders that have been filled
            self.broker.process_filled_orders(filled_orders) 

            #allow strategy to place orders based on current market view. Orders processed next market open
            orders = self.strategy.next()

            #place the orders to the broker who routes them to the exchange
            self.broker.place_orders(orders)

            #value the portfolio
            self.broker.evaluate_portfolio()

            #analyze portfolio stats
            self.analyze_strategy_on_next()

        """End of backtest"""

        #clear all orders currently open on the exchange
        self.broker.clear_orders()
        
        #close all open positions at last close price for each asst
        self.broker.clear_portfolio(on_open = False)

        #provide a final valuation of the portfolio
        self.broker.evaluate_portfolio()

        #analyize strategy statistics
        self.analyze_strategy_on_finish()


if __name__ == "__main__":
    exchange = Exchange()
    broker = Broker(exchange=exchange)
    strategy = Strategy(exchange=exchange,broker=broker)

    source_type = "csv"
    datetime_format = "%Y-%m-%d"
    datetime_column = "DATE"

    tickers = ["AAL","AAP"]

    for ticker in tickers:
        csv_path = r"C:\Users\bktor\Desktop\Python\FastTest\data\{}.csv".format(ticker)
        name = ticker
        exchange.register_asset(Asset(
                ticker,
                source_type = source_type,
                csv_path = csv_path,
                datetime_format = datetime_format,
                datetime_column = datetime_column
            )
        )
    
    fast_test = FastTest(
        exchange=exchange,
        broker=broker,
        strategy=strategy
        )
    
    fast_test.run()