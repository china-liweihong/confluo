#ifndef RPC_DIALOG_SERVER_H_
#define RPC_DIALOG_SERVER_H_

#include "dialog_service.h"
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/server/TThreadedServer.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TServerSocket.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/concurrency/ThreadManager.h>
#include <thrift/concurrency/PlatformThreadFactory.h>
#include <thrift/transport/TTransportUtils.h>
#include <thrift/TToString.h>

#include <thread>

#include "rpc_type_conversions.h"
#include "rpc_configuration_params.h"
#include "dialog_store.h"
#include "dialog_table.h"
#include "logger.h"

using namespace ::apache::thrift;
using namespace ::apache::thrift::protocol;
using namespace ::apache::thrift::transport;
using namespace ::apache::thrift::server;

using boost::shared_ptr;

namespace dialog {
namespace rpc {

class dialog_service_handler : virtual public dialog_serviceIf {
 public:
  typedef dialog_table::fri_result_type adhoc_stream;
  typedef dialog_table::filter_rstream_type predef_stream;
  typedef dialog_table::ffilter_rstream_type combined_stream;
  typedef dialog_table::alert_list::iterator alert_iterator;
  typedef std::pair<alert_iterator, alert_iterator> alert_entry;

  typedef std::map<rpc_iterator_id, adhoc_stream> adhoc_map;
  typedef std::map<rpc_iterator_id, predef_stream> predef_map;
  typedef std::map<rpc_iterator_id, combined_stream> combined_map;
  typedef std::map<rpc_iterator_id, alert_entry> alerts_map;

  dialog_service_handler(dialog_store* store)
      : handler_id_(-1),
        store_(store),
        cur_table_(nullptr),
        iterator_id_(0) {
  }

  virtual void register_handler() {
    handler_id_ = thread_manager::register_thread();
    if (handler_id_ < 0) {
      rpc_management_exception ex;
      ex.msg = "Could not register handler";
      throw ex;
    } else {
      LOG_INFO<< "Registered handler thread " << std::this_thread::get_id() << " as " << handler_id_;
    }
  }

  virtual void deregister_handler() {
    int ret = thread_manager::deregister_thread();
    if (ret < 0) {
      rpc_management_exception ex;
      ex.msg = "Could not deregister handler";
      throw ex;
    } else {
      LOG_INFO << "Deregistered handler thread " << std::this_thread::get_id() << " as " << ret;
    }
  }

  void create_table(const std::string& table_name, const rpc_schema& schema,
      const rpc_storage_mode mode) {
    try {
      store_->add_table(table_name, rpc_type_conversions::convert_schema(schema),
          rpc_type_conversions::convert_mode(mode));
    } catch(management_exception& ex) {
      rpc_management_exception e;
      e.msg = ex.what();
      throw e;
    }
  }

  void set_current_table(rpc_schema& _return, const std::string& table_name) {
    try {
      cur_table_ = store_->get_table(table_name);
      const auto& schema = cur_table_->get_schema().columns();
      _return = rpc_type_conversions::convert_schema(schema);
    } catch(management_exception& ex) {
      rpc_management_exception e;
      e.msg = ex.what();
      throw e;
    }
  }

  void add_index(const std::string& field_name, const double bucket_size) {
    try {
      cur_table_->add_index(field_name, bucket_size);
    } catch(management_exception& ex) {
      rpc_management_exception e;
      e.msg = ex.what();
      throw e;
    }
  }

  void remove_index(const std::string& field_name) {
    try {
      cur_table_->remove_index(field_name);
    } catch(management_exception& ex) {
      rpc_management_exception e;
      e.msg = ex.what();
      throw e;
    }
  }

  void add_filter(const std::string& filter_name,
      const std::string& filter_expr) {
    try {
      cur_table_->add_filter(filter_name, filter_expr);
    } catch(management_exception& ex) {
      rpc_management_exception e;
      e.msg = ex.what();
      throw e;
    } catch(parse_exception& ex) {
      rpc_management_exception e;
      e.msg = ex.what();
      throw e;
    }
  }

  void remove_filter(const std::string& filter_name) {
    try {
      cur_table_->remove_filter(filter_name);
    } catch(management_exception& ex) {
      rpc_management_exception e;
      e.msg = ex.what();
      throw e;
    }
  }

  void add_trigger(const std::string& trigger_name,
      const std::string& filter_name,
      const std::string& trigger_expr) {
    try {
      cur_table_->add_trigger(trigger_name, filter_name, trigger_expr);
    } catch(management_exception& ex) {
      rpc_management_exception e;
      e.msg = ex.what();
      throw e;
    } catch(parse_exception& ex) {
      rpc_management_exception e;
      e.msg = ex.what();
      throw e;
    }
  }

  void remove_trigger(const std::string& trigger_name) {
    try {
      cur_table_->remove_trigger(trigger_name);
    } catch(management_exception& ex) {
      rpc_management_exception e;
      e.msg = ex.what();
      throw e;
    }
  }

  int64_t append(const std::string& data) {
    void* buf = (char*) &data[0];  // XXX: Fix
    return cur_table_->append(buf);
  }

  int64_t append_batch(const rpc_record_batch& batch) {
    record_batch rbatch = rpc_type_conversions::convert_batch(batch);
    return cur_table_->append_batch(rbatch);
  }

  void read(std::string& _return, const int64_t offset,
      const int64_t nrecords) {
    char* data = reinterpret_cast<char*>(cur_table_->read(offset));
    _return.assign(data, nrecords * cur_table_->record_size());
  }

  void adhoc_filter(rpc_iterator_handle& _return,
      const std::string& filter_expr) {
    bool success = false;
    rpc_iterator_id it_id = new_iterator_id();
    try {
      auto res = cur_table_->execute_filter(filter_expr);
      auto ret = adhoc_.insert(std::make_pair(it_id, res));
      success = ret.second;
    } catch (parse_exception& ex) {
      rpc_invalid_operation e;
      e.msg = ex.what();
      throw e;
    }

    if (!success) {
      rpc_invalid_operation e;
      e.msg = "Duplicate rpc_iterator_id assigned";
      throw e;
    }

    adhoc_more(_return, it_id);
  }

  void predef_filter(rpc_iterator_handle& _return,
      const std::string& filter_name, const int64_t begin_ms,
      const int64_t end_ms) {
    rpc_iterator_id it_id = new_iterator_id();
    auto res = cur_table_->query_filter(filter_name, begin_ms, end_ms);
    auto ret = predef_.insert(std::make_pair(it_id, res));
    if (!ret.second) {
      rpc_invalid_operation e;
      e.msg = "Duplicate rpc_iterator_id assigned";
      throw e;
    }

    predef_more(_return, it_id);
  }

  void combined_filter(rpc_iterator_handle& _return,
      const std::string& filter_name,
      const std::string& filter_expr, const int64_t begin_ms,
      const int64_t end_ms) {
    bool success = false;
    rpc_iterator_id it_id = new_iterator_id();
    try {
      auto res = cur_table_->query_filter(filter_name, filter_expr, begin_ms, end_ms);
      auto ret = combined_.insert(std::make_pair(it_id, res));
      success = ret.second;
    } catch (parse_exception& ex) {
      rpc_invalid_operation e;
      e.msg = ex.what();
      throw e;
    }
    if (!success) {
      rpc_invalid_operation e;
      e.msg = "Duplicate rpc_iterator_id assigned";
      throw e;
    }

    combined_more(_return, it_id);
  }

  void alerts_by_time(rpc_iterator_handle& _return, const int64_t begin_ms,
      const int64_t end_ms) {
    rpc_iterator_id it_id = new_iterator_id();
    auto alerts = cur_table_->get_alerts(begin_ms, end_ms);
    auto ret = alerts_.insert(std::make_pair(it_id, std::make_pair(alerts.begin(), alerts.end())));
    if (!ret.second) {
      rpc_invalid_operation e;
      e.msg = "Duplicate rpc_iterator_id assigned";
      throw e;
    }

    alerts_more(_return, it_id);
  }

  void get_more(rpc_iterator_handle& _return,
      const rpc_iterator_descriptor& desc) {
    if (desc.handler_id != handler_id_) {
      rpc_invalid_operation ex;
      ex.msg = "handler_id mismatch";
      throw ex;
    }

    switch (desc.type) {
      case rpc_iterator_type::RPC_ADHOC: {
        adhoc_more(_return, desc.id);
        break;
      }
      case rpc_iterator_type::RPC_PREDEF: {
        predef_more(_return, desc.id);
        break;
      }
      case rpc_iterator_type::RPC_COMBINED: {
        combined_more(_return, desc.id);
        break;
      }
      case rpc_iterator_type::RPC_ALERTS: {
        alerts_more(_return, desc.id);
        break;
      }
    }
  }

  // TODO: get alerts by time and trigger name
  // TODO: subscribe to alerts
  // TODO: active alerts

  int64_t num_records() {
    return cur_table_->num_records();
  }

private:
  rpc_iterator_id new_iterator_id() {
    return iterator_id_++;
  }

  void adhoc_more(rpc_iterator_handle& _return, rpc_iterator_id it_id) {
    // Initialize iterator descriptor
    _return.desc.data_type = rpc_data_type::RPC_RECORD;
    _return.desc.handler_id = handler_id_;
    _return.desc.id = it_id;
    _return.desc.type = rpc_iterator_type::RPC_ADHOC;

    // Read data from iterator
    try {
      auto& res = adhoc_.at(it_id);
      size_t to_read = rpc_configuration_params::ITERATOR_BATCH_SIZE;
      _return.data.reserve(cur_table_->record_size() * to_read);
      size_t i = 0;
      for (; res.has_more() && i < to_read; ++i, ++res) {
        record_t rec = res.get();
        _return.data.append(reinterpret_cast<const char*>(rec.data()), rec.length());
      }
      _return.num_entries = i;
      _return.has_more = res.has_more();
    } catch (std::out_of_range& ex) {
      rpc_invalid_operation e;
      e.msg = "No such iterator";
      throw e;
    }
  }

  void predef_more(rpc_iterator_handle& _return, rpc_iterator_id it_id) {
    // Initialize iterator descriptor
    _return.desc.data_type = rpc_data_type::RPC_RECORD;
    _return.desc.handler_id = handler_id_;
    _return.desc.id = it_id;
    _return.desc.type = rpc_iterator_type::RPC_PREDEF;

    // Read data from iterator
    try {
      auto& res = predef_.at(it_id);
      size_t to_read = rpc_configuration_params::ITERATOR_BATCH_SIZE;
      _return.data.reserve(cur_table_->record_size() * to_read);
      size_t i = 0;
      for (; res.has_more() && i < to_read; ++i, ++res) {
        record_t rec = res.get();
        _return.data.append(reinterpret_cast<const char*>(rec.data()), rec.length());
      }
      _return.num_entries = i;
      _return.has_more = res.has_more();
    } catch (std::out_of_range& ex) {
      rpc_invalid_operation e;
      e.msg = "No such iterator";
      throw e;
    }
  }

  void combined_more(rpc_iterator_handle& _return, rpc_iterator_id it_id) {
    // Initialize iterator descriptor
    _return.desc.data_type = rpc_data_type::RPC_RECORD;
    _return.desc.handler_id = handler_id_;
    _return.desc.id = it_id;
    _return.desc.type = rpc_iterator_type::RPC_COMBINED;

    // Read data from iterator
    try {
      auto& res = combined_.at(it_id);
      size_t to_read = rpc_configuration_params::ITERATOR_BATCH_SIZE;
      _return.data.reserve(cur_table_->record_size() * to_read);
      size_t i = 0;
      for (; res.has_more() && i < to_read; ++i, ++res) {
        record_t rec = res.get();
        _return.data.append(reinterpret_cast<const char*>(rec.data()), rec.length());
      }
      _return.num_entries = i;
      _return.has_more = res.has_more();
    } catch (std::out_of_range& ex) {
      rpc_invalid_operation e;
      e.msg = "No such iterator";
      throw e;
    }
  }

  void alerts_more(rpc_iterator_handle& _return, rpc_iterator_id it_id) {
    // Initialize iterator descriptor
    _return.desc.data_type = rpc_data_type::RPC_ALERT;
    _return.desc.handler_id = handler_id_;
    _return.desc.id = it_id;
    _return.desc.type = rpc_iterator_type::RPC_ALERTS;

    // Read data from iterator
    try {
      auto& res = alerts_.at(it_id);
      size_t to_read = rpc_configuration_params::ITERATOR_BATCH_SIZE;
      size_t i = 0;
      for (auto& it = res.first; it != res.second && i < to_read; ++i, ++it) {
        const alert& a = *it;
        _return.data.append(a.to_string());
        _return.data.push_back('\n');
      }
      _return.num_entries = i;
      _return.has_more = res.first != res.second;
    } catch (std::out_of_range& ex) {
      rpc_invalid_operation e;
      e.msg = "No such iterator";
      throw e;
    }
  }

  rpc_handler_id handler_id_;
  dialog_store* store_;
  dialog_table* cur_table_;

  // Iterator management
  rpc_iterator_id iterator_id_;
  adhoc_map adhoc_;
  predef_map predef_;
  combined_map combined_;
  alerts_map alerts_;
};

class dialog_clone_factory : public dialog_serviceIfFactory {
 public:
  dialog_clone_factory(dialog_store* store)
      : store_(store) {
  }

  virtual ~dialog_clone_factory() {
  }

  virtual dialog_serviceIf* getHandler(const TConnectionInfo& conn_info) {
    shared_ptr<TSocket> sock = boost::dynamic_pointer_cast<TSocket>(
        conn_info.transport);
    LOG_INFO<< "Incoming connection\n"
    << "\t\t\tSocketInfo: " << sock->getSocketInfo() << "\n"
    << "\t\t\tPeerHost: " << sock->getPeerHost() << "\n"
    << "\t\t\tPeerAddress: " << sock->getPeerAddress() << "\n"
    << "\t\t\tPeerPort: " << sock->getPeerPort();
    return new dialog_service_handler(store_);
  }

  virtual void releaseHandler(dialog_serviceIf* handler) {
    delete handler;
  }

 private:
  dialog_store* store_;
};

class dialog_server {
 public:
  static shared_ptr<TThreadedServer> create(dialog_store* store,
                                            const std::string& address,
                                            int port) {
    shared_ptr<dialog_clone_factory> clone_factory(
        new dialog_clone_factory(store));
    shared_ptr<dialog_serviceProcessorFactory> proc_factory(
        new dialog_serviceProcessorFactory(clone_factory));
    shared_ptr<TServerSocket> sock(new TServerSocket(address, port));
    shared_ptr<TBufferedTransportFactory> transport_factory(
        new TBufferedTransportFactory());
    shared_ptr<TBinaryProtocolFactory> protocol_factory(
        new TBinaryProtocolFactory());
    shared_ptr<TThreadedServer> server(
        new TThreadedServer(proc_factory, sock, transport_factory,
                            protocol_factory));
    server->setConcurrentClientLimit(configuration_params::MAX_CONCURRENCY);
    return server;
  }
};

}
}

#endif /* RPC_DIALOG_SERVER_H_ */