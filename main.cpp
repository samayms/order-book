#include <queue>
#include <vector>

class OrderBook {
    struct Order {
        double price;
        int quantity;
        int id; 
    };
    
    
    std::vector<Order&> masterOrders;
    std::priority_queue<int, std::vector<int>, std::less<int>> bids;
    std::priority_queue<int, std::vector<int>, std::greater<int>> asks;

    void marketOrder(bool buy, int qty) {

    }

    void limitOrder() {

    }

    void cancelOrder() {

    }

};