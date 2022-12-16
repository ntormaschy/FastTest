#include "pch.h"
#include "Order.h"
#include "Position.h"
#include "Asset.h"
#include "Exchange.h"
#include "Broker.h"
#include "FastTest.h"

FastTest::FastTest(Exchange &exchangeObj, Broker &brokerObj, Strategy &StrategyObj, bool logging) :
	exchange(exchangeObj), broker(brokerObj), strategy(StrategyObj)
{
	this->logging = logging;
}
void FastTest::reset() {
	this->broker.reset();
	this->exchange.reset();
	this->cash_history.clear();
	this->nlv_history.clear();
}
void FastTest::analyze_step() {
	this->cash_history.push_back(this->broker.cash);
	this->nlv_history.push_back(this->broker.net_liquidation_value);
}
void FastTest::build() {
	this->cash_history.reserve(this->exchange.datetime_index.size());
	this->nlv_history.reserve(this->exchange.datetime_index.size());
}
void FastTest::run() {
	if (this->cash_history.size() != this->exchange.datetime_index.size()) { this->build(); }
	if (this->logging) { printf("RUNNING FASTEST\n"); }
	this->exchange.logging = this->logging;
	this->broker.logging = this->logging;
	this->reset();
	while (this->exchange.step()) {
		//allow exchange to process open orders from previous steps
		if (!this->exchange.orders.empty()) {
			std::vector<std::unique_ptr<Order>> filled_orders = this->exchange.process_orders();
			this->broker.process_filled_orders(std::move(filled_orders));
		}
		//allow strategy to place orders
		this->strategy.next();

		//evaluate the portfolio
		this->broker.evaluate_portfolio();

		//evaluate strategy portfolio
		this->analyze_step();

		//allow the exchange to clean up assets that are done streaming
		std::vector<std::unique_ptr<Order>> canceled_orders = this->exchange.clean_up_market();
		if (!canceled_orders.empty()) {
			this->broker.log_canceled_orders(std::move(canceled_orders));
		}

		//allow exchange to process cheat on close orders
		if (!this->exchange.orders.empty()) {
			std::vector<std::unique_ptr<Order>> filled_orders = this->exchange.process_orders(true);
			this->broker.process_filled_orders(std::move(filled_orders));
		}
		this->step_count++;
	}
}