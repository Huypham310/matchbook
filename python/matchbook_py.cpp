// Python bindings (pybind11). Two levels of API:
//  - Engine.submit / cancel / modify: one call per order, returns the events as Python objects.
//    Convenient, but every call pays ~1 µs of Python <-> C++ overhead.
//  - Engine.submit_many: numpy arrays in, numpy arrays of trades out, one call for a whole
//    batch, so a backtest can push thousands of orders through at C++ speed.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "matchbook/engine.hpp"

namespace py = pybind11;
using namespace matchbook;

namespace {

std::string event_repr(const Event& e) {
    static const char* names[] = {"Trade", "Rested", "Cancelled", "Rejected"};
    std::string s = std::string("Event(") + names[static_cast<int>(e.type)] + ", id=" +
                    std::to_string(e.id);
    if (e.type == EventType::Trade) s += ", maker_id=" + std::to_string(e.maker_id);
    if (e.type != EventType::Rejected) {
        s += ", price=" + std::to_string(e.price) + ", qty=" + std::to_string(e.qty);
    }
    return s + ")";
}

/// A copy of the engine's events for this call (the engine reuses its buffer).
std::vector<Event> events_of(const MatchingEngine& e) { return e.events(); }

}  // namespace

PYBIND11_MODULE(_matchbook, m) {
    m.doc() = "C++20 limit order book and price-time priority matching engine";

    py::enum_<Side>(m, "Side").value("BUY", Side::Buy).value("SELL", Side::Sell);
    py::enum_<OrderType>(m, "OrderType")
        .value("LIMIT", OrderType::Limit)
        .value("MARKET", OrderType::Market);
    py::enum_<TimeInForce>(m, "TimeInForce")
        .value("GTC", TimeInForce::GTC)
        .value("IOC", TimeInForce::IOC)
        .value("FOK", TimeInForce::FOK);
    py::enum_<EventType>(m, "EventType")
        .value("TRADE", EventType::Trade)
        .value("RESTED", EventType::Rested)
        .value("CANCELLED", EventType::Cancelled)
        .value("REJECTED", EventType::Rejected);
    py::enum_<RejectReason>(m, "RejectReason")
        .value("BAD_QUANTITY", RejectReason::BadQuantity)
        .value("DUPLICATE_ID", RejectReason::DuplicateId)
        .value("BOOK_FULL", RejectReason::BookFull)
        .value("UNKNOWN_ORDER", RejectReason::UnknownOrder);

    py::class_<Event>(m, "Event")
        .def_readonly("type", &Event::type)
        .def_readonly("id", &Event::id, "taker / subject order id")
        .def_readonly("maker_id", &Event::maker_id, "resting order id (trades only)")
        .def_readonly("price", &Event::price)
        .def_readonly("qty", &Event::qty)
        .def_readonly("reason", &Event::reason)
        .def("__eq__", [](const Event& a, const Event& b) { return a == b; })
        .def("__repr__", &event_repr);

    py::class_<MatchingEngine>(m, "Engine")
        .def(py::init<std::size_t, std::size_t>(), py::arg("max_orders") = 1u << 20,
             py::arg("max_levels") = 1u << 16)
        .def(
            "submit",
            [](MatchingEngine& e, OrderId id, Side side, Price price, Qty qty, OrderType type,
               TimeInForce tif) {
                e.submit({id, side, type, tif, price, qty});
                return events_of(e);
            },
            py::arg("id"), py::arg("side"), py::arg("price"), py::arg("qty"),
            py::arg("type") = OrderType::Limit, py::arg("tif") = TimeInForce::GTC,
            "Submit one order; returns its events (trades, rested, cancelled or rejected).")
        .def(
            "cancel",
            [](MatchingEngine& e, OrderId id) {
                e.cancel(id);
                return events_of(e);
            },
            py::arg("id"))
        .def(
            "modify",
            [](MatchingEngine& e, OrderId id, Price price, Qty qty) {
                e.modify(id, price, qty);
                return events_of(e);
            },
            py::arg("id"), py::arg("price"), py::arg("qty"))
        .def(
            "submit_many",
            [](MatchingEngine& e, py::array_t<std::uint64_t, py::array::c_style> ids,
               py::array_t<std::int8_t, py::array::c_style> sides,
               py::array_t<std::int64_t, py::array::c_style> prices,
               py::array_t<std::int64_t, py::array::c_style> qtys,
               py::array_t<std::int8_t, py::array::c_style> types,
               py::array_t<std::int8_t, py::array::c_style> tifs) {
                const auto n = ids.size();
                if (sides.size() != n || prices.size() != n || qtys.size() != n ||
                    types.size() != n || tifs.size() != n) {
                    throw py::value_error("all arrays must have the same length");
                }
                auto id = ids.unchecked<1>();
                auto sd = sides.unchecked<1>();
                auto px = prices.unchecked<1>();
                auto qt = qtys.unchecked<1>();
                auto ty = types.unchecked<1>();
                auto tf = tifs.unchecked<1>();
                std::vector<std::uint64_t> taker, maker;
                std::vector<std::int64_t> tprice, tqty;
                std::size_t rejected = 0;
                {
                    // no Python objects touched in here: let other Python threads run
                    py::gil_scoped_release release;
                    for (py::ssize_t i = 0; i < n; ++i) {
                        if (sd(i) > 1 || ty(i) > 1 || tf(i) > 2) {
                            ++rejected;  // out-of-range enum bytes are refused, not cast
                            continue;
                        }
                        e.submit({id(i), static_cast<Side>(sd(i)), static_cast<OrderType>(ty(i)),
                                  static_cast<TimeInForce>(tf(i)), px(i), qt(i)});
                        for (const Event& ev : e.events()) {
                            if (ev.type == EventType::Trade) {
                                taker.push_back(ev.id);
                                maker.push_back(ev.maker_id);
                                tprice.push_back(ev.price);
                                tqty.push_back(ev.qty);
                            } else if (ev.type == EventType::Rejected) {
                                ++rejected;
                            }
                        }
                    }
                }
                py::dict out;
                out["taker_id"] = py::array_t<std::uint64_t>(taker.size(), taker.data());
                out["maker_id"] = py::array_t<std::uint64_t>(maker.size(), maker.data());
                out["price"] = py::array_t<std::int64_t>(tprice.size(), tprice.data());
                out["qty"] = py::array_t<std::int64_t>(tqty.size(), tqty.data());
                out["rejected"] = rejected;
                return out;
            },
            py::arg("ids"), py::arg("sides"), py::arg("prices"), py::arg("qtys"),
            py::arg("types"), py::arg("tifs"),
            "Submit a batch (numpy arrays, enums as small ints) and get every trade back as "
            "arrays: taker_id, maker_id, price, qty, plus a count of rejected orders.")
        .def_property_readonly("best_bid",
                               [](const MatchingEngine& e) { return e.book().best_bid(); })
        .def_property_readonly("best_ask",
                               [](const MatchingEngine& e) { return e.book().best_ask(); })
        .def(
            "depth",
            [](const MatchingEngine& e, Side side, std::size_t n) {
                std::vector<std::tuple<Price, Qty, std::uint32_t>> out;
                for (const auto& l : e.book().depth(side, n)) out.emplace_back(l.price, l.qty, l.orders);
                return out;
            },
            py::arg("side"), py::arg("levels") = 5,
            "Top levels of one side, best first: (price, total qty, order count).")
        .def_property_readonly("order_count",
                               [](const MatchingEngine& e) { return e.book().order_count(); })
        .def_property_readonly("trade_count", &MatchingEngine::trade_count);
}
