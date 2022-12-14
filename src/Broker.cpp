#ifdef _WIN32
#include "pch.h"
#include <Windows.h>
#else
#include <sys/time.h>
#include <cstring>
#endif 
#include <deque>
#include <memory>
#include "Order.h"
#include "Position.h"
#include "Asset.h"
#include "Exchange.h"
#include "Broker.h"

void __Broker::reset() {
	this->cash = 100000;
	this->order_history.clear();
	this->portfolio.clear();
	this->position_history.clear();
	memset(&this->cash_history[0], 0, this->cash_history.size() * sizeof this->cash_history[0]);
	memset(&this->cash_history[0], 0, this->cash_history.size() * sizeof this->cash_history[0]);
	this->position_counter = 0;
	this->order_counter = 0;
	this->unrealized_pl = 0;
	this->realized_pl = 0;
	this->net_liquidation_value = 0;
}

void __Broker::clean_up(){
	size_t history_size = this->nlv_history.size();
	size_t position_count = this->position_history.size();
	long backtest_duration_seconds = *this->__exchange.datetime_index.end() - this->__exchange.datetime_index[0];
	long backtest_duration_years = backtest_duration_seconds / 3.154e7;

	this->perfomance.time_in_market /= history_size;
	this->perfomance.average_time_in_market /= position_count;

	this->perfomance.winrate /= position_count;
	this->perfomance.pl = this->realized_pl;
	this->perfomance.average_return = this->realized_pl / position_count;
	this->perfomance.cagr = pow((*this->nlv_history.end() / this->nlv_history[0]),(1/backtest_duration_years)) - 1;

}

void __Broker::build(){
	size_t size = this->__exchange.datetime_index.size();
	this->cash_history.resize(size);
	this->nlv_history.resize(size);
	this->margin_history.resize(size);

}
void __Broker::analyze_step() {
	unsigned int index = this->__exchange.current_index-1;
	this->cash_history[index] = this->cash;
	this->nlv_history[index] = this->net_liquidation_value;
	this->margin_history[index] = this->margin_loan;

	if(this->portfolio.size() > 0){
		this->perfomance.time_in_market++;
	}
}

float __Broker::get_net_liquidation_value() {
	float nlv = 0;
	for (auto& position : this->portfolio) {
		nlv += position.second.liquidation_value();
	}
	nlv -= this->margin_loan;
	return nlv;
}

void __Broker::open_position(std::unique_ptr<Order> &order) {
	float order_fill_price = order->fill_price;
	float fill_price;

	if(this->has_slippage){
		if(order->units > 0){
			fill_price = order_fill_price * (1 + this->slippage);
			this->total_slippage += ((fill_price - order_fill_price) * order->units);
			order_fill_price = fill_price;
		}
		else if (order->units < 0){
			fill_price = order_fill_price * (1 - this->slippage);
			this->total_slippage += ((fill_price - order_fill_price) * order->units);
			order_fill_price = fill_price;
		}
	}

	Position new_position = Position{
		this->position_counter,
		order->asset_id,
		order->units,
		order_fill_price,
		order->order_fill_time
	};

	if(!this->margin){
		this->cash -= order->units*order_fill_price;
	}
	else {
		//set the appropriate margin requirment for the position
		float margin_req_mid;
		if(new_position.units < 0){
			margin_req_mid = this->short_margin_req;
		}
		else{
			margin_req_mid = this->margin_req;
		}
		//if is margin account, calculate the colateral needed to maintain the trade and the 
		//margin loan provided by the broker. Subtract collateral from the cash available.
		float collateral = abs(margin_req_mid * order->units*order_fill_price);
		float loan = abs((1-margin_req_mid) * order->units*order_fill_price);
		
		//remove collateral required for the position from the cash in the account
		this->cash -= collateral;

		//if the position is short, credit the account with the cash from the sale of borrowd securities
		if(new_position.units < 0){
			this->cash -= order->units*order_fill_price;
		}
		this->margin_loan += loan;

		new_position.collateral = collateral;
		new_position.margin_loan = loan;
	}
	
	if(this->has_commission){
		this->cash -= this->commission;
		this->total_commission += this->commission;
	}

	this->position_counter++;

	if (this->logging) { log_open_position(new_position); }
	this->portfolio[order->asset_id] = new_position;
}

void __Broker::close_position(Position &existing_position, float order_fill_price, timeval order_fill_time) {
	//note: this function does not remove the position from the portfolio so this should not
	//be called directly in order to close a position. Close position through a appropriate order.
	float fill_price;

	if(this->has_slippage){
		if(existing_position.units < 0){
			fill_price = order_fill_price * (1 + this->slippage);
			this->total_slippage += ((fill_price - order_fill_price) * existing_position.units);
			order_fill_price = fill_price;
		}
		else if (existing_position.units > 0){
			fill_price = order_fill_price * (1 - this->slippage);
			this->total_slippage += ((fill_price - order_fill_price) * existing_position.units);
			order_fill_price = fill_price;
		}
	}
	
	existing_position.close(order_fill_price, order_fill_time);
	if (this->logging) { log_close_position(existing_position); }
	this->position_history.push_back(existing_position);
	this->realized_pl += existing_position.realized_pl;

	this->perfomance.average_time_in_market += existing_position.position_close_time - existing_position.position_create_time;
	if(existing_position.realized_pl > 0){this->perfomance.winrate++;}

	if(this->has_commission){
		this->cash -= this->commission;
		this->total_commission += this->commission;
	}

	if(this->margin){
		//free the collateral from the position and remove the margin loan
		this->cash += existing_position.collateral;
		this->margin_loan -= existing_position.margin_loan;

		//if closing a short position have to buy back the position at the filled price
		if(existing_position.units < 0){
			this->cash += existing_position.units * order_fill_price;
		}
	}
	else{
		this->cash += existing_position.units * order_fill_price;
	}
}

void __Broker::reduce_position(Position &existing_position, std::unique_ptr<Order>& order) {
	existing_position.reduce(order->fill_price, order->units);
	this->cash += abs(order->units) * order->fill_price;
}

void __Broker::increase_position(Position &existing_position, std::unique_ptr<Order>& order) {
	existing_position.increase(order->fill_price, order->units);
	this->cash -= order->units * order->fill_price;
}

bool __Broker::cancel_order(std::unique_ptr<Order>& order_cancel) {
	std::unique_ptr<Order> canceled_order = this->__exchange.cancel_order(order_cancel);
	canceled_order->order_state = CANCELED;
	this->order_history.push_back(std::move(canceled_order));
	return true;
}

void __Broker::log_canceled_orders(std::vector<std::unique_ptr<Order>> cleared_orders) {
	for (auto& order : cleared_orders) {
		this->order_history.push_back(std::move(order));
	}
}

bool __Broker::cancel_orders(unsigned int asset_id) {
	for (auto& order : this->__exchange.orders) {
		if (order->asset_id != asset_id) { continue; }
		if (!this->cancel_order(order)) {
			return false;
		}
	}
	return true;
}

void __Broker::clear_child_orders(Position& existing_position) {
	for (auto& order : this->__exchange.orders) {
		if (order->order_type == STOP_LOSS_ORDER || order->order_type == TAKE_PROFIT_ORDER) {
			StopLossOrder* stop_loss = static_cast <StopLossOrder*>(order.get());
			if (*stop_loss->order_parent.member.parent_position == existing_position) {
				this->__exchange.cancel_order(order);
			}
		}
	}
}

MARGIN_CHECK __Broker::check_margin() noexcept{
	if(!margin){
		return VALID_ACCOUNT_STATUS;
	}
	//check to see if nlv is below broker's margin account requirment 
	if(this->net_liquidation_value < this->minimum_nlv){
		return NLV_BELOW_BROKER_REQ;
	}
	if(this->cash < 0){
		return MARGIN_CALL;
	}
	return VALID_ACCOUNT_STATUS;
}

#ifdef CHECK_ORDER
ORDER_CHECK __Broker::check_stop_loss_order(const StopLossOrder* stop_loss_order) {
	OrderParent parent = stop_loss_order->order_parent;
	if (parent.type == ORDER) {
		Order* parent_order = parent.member.parent_order;
		if (parent_order->asset_id != stop_loss_order->asset_id) { return INVALID_ASSET; }
		if (parent_order->order_type > 1) { return INVALID_PARENT_ORDER; }
		if (parent_order->units*stop_loss_order->units > 0) { return INVALID_ORDER_SIDE; }
	}
	else {
		Position* parent_position = parent.member.parent_position;
		if (parent_position->asset_id != stop_loss_order->asset_id) { return INVALID_ASSET; }
		if (parent_position->units*stop_loss_order->units > 0) { return INVALID_ORDER_SIDE; }
	}
	return VALID_ORDER;
}

ORDER_CHECK __Broker::check_take_profit_order(const TakeProfitOrder* take_profit_order){
	OrderParent parent = take_profit_order->order_parent;
	if (parent.type == ORDER) {
		Order* parent_order = parent.member.parent_order;
		if (parent_order->asset_id != take_profit_order->asset_id) { return INVALID_ASSET; }
		if (parent_order->order_type > 1) { return INVALID_PARENT_ORDER; }
		if (parent_order->units*take_profit_order->units > 0) { return INVALID_ORDER_SIDE; }
	}
	else {
		Position* parent_position = parent.member.parent_position;
		if (parent_position->asset_id != take_profit_order->asset_id) { return INVALID_ASSET; }
		if (parent_position->units*take_profit_order->units > 0) { return INVALID_ORDER_SIDE; }
	}
	return VALID_ORDER;
}

ORDER_CHECK __Broker::check_market_order(const MarketOrder* market_order) {
	unsigned int asset_id = market_order->asset_id;
	float units = market_order->units;
	float market_price = this->__exchange._get_market_price(asset_id);
	if(!this->_position_exists(asset_id)){
		if(market_order->units * market_price > this->cash){
			return INVALID_ORDER_COLLATERAL;
		};
	}
	else if (this->portfolio[asset_id].units * units > 0){
		if(market_order->units * market_price > this->cash){
			return INVALID_ORDER_COLLATERAL;
		};
	}
	return VALID_ORDER;
}

ORDER_CHECK __Broker::check_order(const std::unique_ptr<Order>& new_order) {
	//check to see if the asset exsists on the exchange
	if (this->__exchange.market.count(new_order->asset_id) == 0) { return INVALID_ASSET; }
	
	//An order can not have units set to 0
	if (new_order->units == 0) { return INVALID_ORDER_UNITS; }

	ORDER_CHECK order_code;
	switch (new_order->order_type) {
		case MARKET_ORDER: {
			MarketOrder* market_order = static_cast <MarketOrder*>(new_order.get());
			order_code = check_market_order(market_order);
			break;
		}
		case LIMIT_ORDER: {
			order_code = VALID_ORDER;
			break;
		}
		case STOP_LOSS_ORDER: {
			StopLossOrder* stop_loss_order = static_cast <StopLossOrder*>(new_order.get());
			order_code = check_stop_loss_order(stop_loss_order);
			break;
		}
		case TAKE_PROFIT_ORDER: {
			TakeProfitOrder* take_profit_order = static_cast <TakeProfitOrder*>(new_order.get());
			order_code = check_take_profit_order(take_profit_order);
			break;
		}
	}
	if (order_code != VALID_ORDER) { return order_code; }
	for (auto& order_on_fill : new_order->orders_on_fill) {
		this->check_order(order_on_fill);
	}
	return order_code;
}
#endif
OrderState __Broker::send_order(std::unique_ptr<Order> new_order) {
	new_order->order_state = ACCEPETED;
	new_order->order_create_time = this->__exchange.current_time;
	new_order->order_id = this->order_counter;
	this->__exchange.place_order(std::move(new_order));
	this->order_counter++;
	return ACCEPETED;
}
OrderState __Broker::_place_market_order(unsigned int asset_id, float units, bool cheat_on_close) {
	std::unique_ptr<Order> order(new MarketOrder(
		asset_id,
		units,
		cheat_on_close
	));
#ifdef CHECK_ORDER
	if (check_order(order) != VALID_ORDER) {
		order->order_state = BROKER_REJECTED;
		this->order_history.push_back(std::move(order));
		return BROKER_REJECTED;
	}
#endif
	return this->send_order(std::move(order));
}
OrderState __Broker::_place_limit_order(unsigned int asset_id, float units, float limit, bool cheat_on_close) {
	std::unique_ptr<Order> order(new LimitOrder(
		asset_id,
		units,
		limit,
		cheat_on_close
	));
#ifdef CHECK_ORDER
	if (check_order(order) != VALID_ORDER) {
		order->order_state = BROKER_REJECTED;
		this->order_history.push_back(std::move(order));
		return BROKER_REJECTED;
	}
#endif
	return this->send_order(std::move(order));
}
void __Broker::process_filled_orders(std::vector<std::unique_ptr<Order>> orders_filled) {
	for (auto& order : orders_filled) {
		//no position exists, create new open position
		if (!this->_position_exists(order->asset_id)) {
			this->open_position(order);
		}
		else {
			Position &existing_position = this->portfolio[order->asset_id];
			//sum of existing position units and order units is 0. Close existing position
			if (existing_position.units + order->units == 0) {
				this->close_position(existing_position, order->fill_price, order->order_fill_time);
				this->portfolio.erase(order->asset_id);
			}
			//order is same direction as existing position. Increase existing position
			else if (existing_position.units * order->units > 0) {
				this->increase_position(existing_position, order);
			}
			//order is in opposite direction as existing position. Reduce existing position
			else {
				if(abs(order->units) < abs(existing_position.units)){
					this->reduce_position(existing_position, order);
				}
				else{
					this->close_position(existing_position, order->fill_price, order->order_fill_time);
					this->portfolio.erase(order->asset_id);
					order->units = order->units + existing_position.units;
					this->open_position(order);
				}
			}
		}
		this->order_history.push_back(std::move(order));
	}
}
std::deque<std::unique_ptr<Order>>& __Broker::open_orders() {
	return this->__exchange.orders;
}
bool __Broker::_position_exists(unsigned int asset_id) {
	return this->portfolio.count(asset_id) > 0;
}
void __Broker::set_cash(float cash) {
	this->cash = cash;
}
void __Broker::log_open_position(Position &position) {
	memset(this->time, 0, sizeof this->time);
	timeval_to_char_array(&position.position_create_time, this->time, sizeof(this->time));
	printf("%s: OPENING POSITION: asset_id: %i, units: %f, avg_price: %f\n",
		this->time,
		position.asset_id,
		position.units,
		position.average_price
	);
}
void __Broker::log_close_position(Position &position) {
	memset(this->time, 0, sizeof this->time);
	timeval_to_char_array(&position.position_close_time, this->time, sizeof(this->time));
	printf("%s: CLOSING POSITION: asset_id: %i, units: %f, close_price: %f\n",
		this->time,
		position.asset_id,
		position.units,
		position.close_price
	);
}
void * CreateBrokerPtr(void *exchange_ptr, bool logging, bool margin) {
	__Exchange *__exchange_ref = static_cast<__Exchange *>(exchange_ptr);
	return new __Broker(*__exchange_ref, logging, margin);
}
void DeleteBrokerPtr(void *ptr) {
	__Broker * __broker_ref = static_cast<__Broker *>(ptr);
	delete __broker_ref;
}
void reset_broker(void *broker_ptr) {
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	__broker_ref->reset();
}
void build_broker(void *broker_ptr) {
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	__broker_ref->build();
}
float get_cash(void *broker_ptr){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->cash;
}
float get_nlv(void *broker_ptr){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->net_liquidation_value;
}
float get_unrealized_pl(void *broker_ptr){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->unrealized_pl;
}
float get_realized_pl(void *broker_ptr){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->realized_pl;
}
int get_order_count(void *broker_ptr) {
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->order_history.size();
}
int get_position_count(void *broker_ptr) {
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->position_history.size();
}
int get_open_position_count(void *broker_ptr){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->portfolio.size();
}
bool position_exists(void *broker_ptr, unsigned int asset_id){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->_position_exists(asset_id);
}
OrderState place_market_order(void *broker_ptr, unsigned int asset_id, float units, bool cheat_on_close) {
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->_place_market_order(asset_id, units, cheat_on_close);
}
OrderState place_limit_order(void *broker_ptr, unsigned int asset_id, float units, float limit, bool cheat_on_close){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->_place_limit_order(asset_id, units, limit, cheat_on_close);
}
OrderState position_add_stoploss(void *broker_ptr, void * position_ptr, float units, float stop_loss, bool cheat_on_close){
		__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
		Position * __position_ref = static_cast<Position *>(position_ptr);
		return __broker_ref->place_stoploss_order(
			__position_ref, units, stop_loss, cheat_on_close
		);
}
OrderState order_add_stoploss(void *broker_ptr, unsigned int order_id, float units, float stop_loss, bool cheat_on_close){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	for (auto& order : __broker_ref->__exchange.orders){
		if(order->order_id == order_id){
			order->add_stop_loss(stop_loss, units);
		}
	}
}
size_t broker_get_history_length(void *broker_ptr){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->nlv_history.size();
}
float * broker_get_nlv_history(void *broker_ptr) {
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->nlv_history.data();
}
float * broker_get_cash_history(void *broker_ptr) {
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->cash_history.data();
}
float * broker_get_margin_history(void *broker_ptr) {
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return __broker_ref->margin_history.data();
}
void get_order_history(void *broker_ptr, OrderArray *order_history) {
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	int number_orders = order_history->number_orders;
	for (int i = 0; i < number_orders; i++) {
		OrderStruct &order_struct_ref = *order_history->ORDER_ARRAY[i];
		__broker_ref->order_history[i]->to_struct(order_struct_ref);
	}
}
void get_position_history(void *broker_ptr, PositionArray *position_history) {
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	int number_positions = position_history->number_positions;
	for (int i = 0; i < number_positions; i++) {
		PositionStruct &position_struct_ref = *position_history->POSITION_ARRAY[i];
		__broker_ref->position_history[i].to_struct(position_struct_ref);
	}
}
void get_positions(void *broker_ptr, PositionArray *positions){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	int number_positions = __broker_ref->portfolio.size();
	int i = 0;
	for (auto &kvp : __broker_ref->portfolio){
		PositionStruct &position_struct_ref = *positions->POSITION_ARRAY[i];
		__broker_ref->portfolio[kvp.first].to_struct(position_struct_ref);
		i++;
	}
}
void get_position(void *broker_ptr, unsigned int assset_id, PositionStruct *position){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	int number_positions = __broker_ref->portfolio.size();
	if(__broker_ref->portfolio.count(assset_id) == 0){return;}
	__broker_ref->portfolio[assset_id].to_struct(*position);
}
void get_orders(void *broker_ptr, OrderArray *orders){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	int number_orders = __broker_ref->__exchange.orders.size();
	for (int i = 0; i < number_orders; i++) {
		OrderStruct &order_struct_ref = *orders->ORDER_ARRAY[i];
		std::unique_ptr<Order>& open_order = __broker_ref->__exchange.orders[i];
		order_ptr_to_struct(open_order, order_struct_ref);
	}
}
void * get_position_ptr(void *broker_ptr, unsigned int asset_id){
	__Broker * __broker_ref = static_cast<__Broker *>(broker_ptr);
	return &__broker_ref->portfolio[asset_id];
}