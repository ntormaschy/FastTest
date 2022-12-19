#pragma once
#ifndef ORDER_H // include guard
#define ORDER_H
#ifdef ASSET_EXPORTS
#define ORDER_API __declspec(dllexport)
#else
#define ORDER_API __declspec(dllimport)
#endif

#include "pch.h"
#include <iostream>
#ifdef _WIN32
#include <WinSock2.h>
#else
#include <sys/time.h>
#endif 
#include <math.h>
#include <memory>
#include "Position.h"
#include "utils_time.h"
#include <vector>

enum OrderState {
	ACCEPETED,
	OPEN,
	FILLED,
	CANCELED,
	BROKER_REJECTED
};
enum OrderType {
	MARKET_ORDER,
	LIMIT_ORDER,
	STOP_LOSS_ORDER,
	TAKE_PROFIT_ORDER
};
enum OrderParentType {
	POSITION = 0,
	ORDER = 1
};

class Order
{
public:
	//track order type (allow for polymorphism so can easily store orders in vecotr)
	OrderType order_type;

	//track state of the order
	OrderState order_state = OPEN;

	//specify wether or not a order can be executed at close of the view it was placed
	bool cheat_on_close = false;

	//once we have seen that the order is not being executed at the current view, it can then
	//be executed at time after
	bool alive = false;


	float units;			//number of units to buy/sell
	float fill_price;		//price the order was filled at
	unsigned int order_id;  //unique identifier for the order
	UINT asset_id; //underlying asset for the order


	timeval order_create_time; //the time the order was placed on the exchange
	timeval order_fill_time;   //the time that the order was filled by the exchange

	std::vector<std::unique_ptr<Order>> orders_on_fill; //container for orders to execute once the parent order as filled

	const char* get_order_type();
	void create(timeval order_create_time);
	void fill(float market_price, timeval fill_time);
	void add_stop_loss(float price, float units = NAN);

	Order(OrderType _OrderType, UINT asset_id, float units, bool cheat_on_close = false) {
		this->order_type = _OrderType;
		this->asset_id = asset_id;
		this->units = units;
		this->cheat_on_close = cheat_on_close;
	}
	Order() = default;
	virtual ~Order() {};
	Order(const Order&) = delete;
	Order& operator =(const Order&) = delete;

	friend bool operator==(const Order& lhs, const Order& rhs)
	{
		return &lhs == &rhs;
	}
};

class MarketOrder : public Order
{
public:
	MarketOrder(UINT asset_id, float units, bool cheat_on_close = false)
		: Order(MARKET_ORDER, asset_id, units, cheat_on_close)
	{}
};
class LimitOrder : public Order
{
public:
	float limit;
	LimitOrder(UINT asset_id, float units, float limit, bool cheat_on_close = false)
		: Order(LIMIT_ORDER, asset_id, units, cheat_on_close) {
		this->limit = limit;
	}
};
struct OrderParent {
	OrderParentType type;
	union {
		Order* parent_order;
		Position* parent_position;
	} member;
};
class StopLossOrder : public Order
{
public:
	OrderParent order_parent;
	float stop_loss;
	StopLossOrder(Order *parent_order, float units, float stop_loss, bool cheat_on_close = false)
		: Order(STOP_LOSS_ORDER, parent_order->asset_id, units, cheat_on_close) {
		this->order_parent.member.parent_order = parent_order;
		this->order_parent.type = ORDER;
		this->stop_loss = stop_loss;
	}
	StopLossOrder(Position *parent_position, float units, float stop_loss, bool cheat_on_close = false)
		: Order(STOP_LOSS_ORDER, parent_position->asset_id, units, cheat_on_close) {
		this->order_parent.member.parent_position = parent_position;
		this->order_parent.type = POSITION;
		this->stop_loss = stop_loss;
	}
};
extern "C" {
	ORDER_API OrderType order_type(void *order_ptr);
}
#endif