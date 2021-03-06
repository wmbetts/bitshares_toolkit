#define BOOST_TEST_MODULE BtsXtClientTests
#include <boost/test/unit_test.hpp>
#include <boost/program_options.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/scope_exit.hpp>
 
#include <sstream>
#include <fstream>
#include <iomanip>
#include <iostream>

#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <fc/thread/thread.hpp>
#include <fc/filesystem.hpp>
#include <fc/network/ip.hpp>
#include <fc/io/json.hpp>

#include <fc/interprocess/process.hpp>

#include <fc/reflect/variant.hpp>
#include <bts/wallet/wallet.hpp>
#include <bts/rpc/rpc_client.hpp>
#include <bts/blockchain/asset.hpp>

struct bts_xt_client_test_config 
{
  static fc::path bts_client_exe;
  static fc::path bts_server_exe;
  static fc::path config_directory;
  static uint16_t base_rpc_port;

  bts_xt_client_test_config() 
  {
    // parse command-line options
    boost::program_options::options_description option_config("Allowed options");
    option_config.add_options()("bts-client-exe", boost::program_options::value<std::string>(), "full path to the executable to test")
                               ("bts-server-exe", boost::program_options::value<std::string>(), "full path to the server executable for testing client-server mode")
                               ("extra-help", "display this help message");


    boost::program_options::variables_map option_variables;
    try
    {
      boost::program_options::store(boost::program_options::command_line_parser(boost::unit_test::framework::master_test_suite().argc, 
                                                                                boost::unit_test::framework::master_test_suite().argv).
        options(option_config).run(), option_variables);
      boost::program_options::notify(option_variables);
    }
    catch (boost::program_options::error&)
    {
      std::cerr << "Error parsing command-line options\n\n";
      std::cerr << option_config << "\n";
      exit(1);
    }

    if (option_variables.count("extra-help"))
    {
      std::cout << option_config << "\n";
      exit(0);
    }

    if (option_variables.count("bts-client-exe"))
      bts_client_exe = option_variables["bts-client-exe"].as<std::string>().c_str();

    if (option_variables.count("bts-server-exe"))
      bts_server_exe = option_variables["bts-server-exe"].as<std::string>().c_str();

    std::cout << "Testing " << bts_client_exe.string() << "\n";
    std::cout << "Using config directory " << config_directory.string() << "\n";
    fc::create_directories(config_directory);

    boost::unit_test::unit_test_log.set_threshold_level(boost::unit_test::log_messages);
  }
};
fc::path bts_xt_client_test_config::bts_client_exe = "e:/Invictus/vs12_bt/programs/bts_xt/Debug/bts_xt_client.exe";
fc::path bts_xt_client_test_config::bts_server_exe = "e:/Invictus/vs12_bt/programs/bts_xt/Debug/bts_xt_server.exe";
fc::path bts_xt_client_test_config::config_directory = fc::temp_directory_path() / "bts_xt_client_tests";
uint16_t bts_xt_client_test_config::base_rpc_port = 20100;

#define RPC_USERNAME "test"
#define RPC_PASSWORD "test"
#define WALLET_PASPHRASE "testtest"
#define INITIAL_BALANCE 100000000


BOOST_GLOBAL_FIXTURE(bts_xt_client_test_config);


struct genesis_block_config
{
   genesis_block_config():supply(0),blockheight(0){}
   double                                            supply;
   uint64_t                                          blockheight;
   std::vector< std::pair<bts::blockchain::pts_address,uint64_t> > balances;
};
FC_REFLECT( genesis_block_config, (supply)(balances) )

struct bts_server_process_info
{
  fc::process_ptr server_process;
  fc::future<void> stdout_reader_done;
  fc::future<void> stderr_reader_done;
  ~bts_server_process_info()
  {
    server_process->kill();
    if (stdout_reader_done.valid() && !stdout_reader_done.ready())
      stdout_reader_done.wait();
    if (stderr_reader_done.valid() && !stderr_reader_done.ready())
      stderr_reader_done.wait();
  }
};
typedef std::shared_ptr<bts_server_process_info> bts_server_process_info_ptr;

bts_server_process_info_ptr launch_bts_server_process(const genesis_block_config& genesis_block,
                                                      const fc::ecc::private_key& trustee_key)
{
  bts_server_process_info_ptr server_process_info = std::make_shared<bts_server_process_info>();
  server_process_info->server_process = std::make_shared<fc::process>();
  
  std::vector<std::string> options;
  options.push_back("--trustee-address");
  options.push_back(bts::blockchain::address(trustee_key.get_public_key()));

  fc::path server_config_dir = bts_xt_client_test_config::config_directory / "BitSharesX_Server";
  fc::remove_all(server_config_dir);
  fc::create_directories(server_config_dir);

  fc::json::save_to_file(genesis_block, server_config_dir / "genesis.json", true);

  server_process_info->server_process->exec(bts_xt_client_test_config::bts_server_exe, options, server_config_dir);

  std::shared_ptr<std::ofstream> stdouterrfile = std::make_shared<std::ofstream>((server_config_dir / "stdouterr.txt").string().c_str());
  fc::buffered_istream_ptr out_stream = server_process_info->server_process->out_stream();
  fc::buffered_istream_ptr err_stream = server_process_info->server_process->err_stream();
  server_process_info->stdout_reader_done = fc::async([out_stream,stdouterrfile]()
    {
      char buf[1024];
      for (;;)
      {
        size_t bytes_read = out_stream->readsome(buf, sizeof(buf));
        if (!bytes_read)
          break;
        stdouterrfile->write(buf, bytes_read);
      }
    });
  server_process_info->stderr_reader_done = fc::async([err_stream,stdouterrfile]()
    {
      char buf[1024];
      for (;;)
      {
        size_t bytes_read = err_stream->readsome(buf, sizeof(buf));
        if (!bytes_read)
          break;
        stdouterrfile->write(buf, bytes_read);
      }
    });
  return server_process_info;
}

fc::process_ptr launch_bts_client_process(uint32_t process_number, uint16_t rpc_port, 
                                          const fc::ecc::private_key& trustee_key, 
                                          bool act_as_trustee)
{
  fc::process_ptr bts_client_process(std::make_shared<fc::process>());
  std::vector<std::string> options;

  std::ostringstream numbered_config_dir_name;
  numbered_config_dir_name << "BitSharesX_" << std::setw(3) << std::setfill('0') << process_number;
  fc::path numbered_config_dir = bts_xt_client_test_config::config_directory / numbered_config_dir_name.str();
  fc::remove_all(numbered_config_dir);
  fc::create_directories(numbered_config_dir);

  // create a wallet in that directory
  // we could (and probably should) make bts_xt_client create the wallet, 
  // but if we ask it to create the wallet
  // it will interactively prompt for passwords which is a big hassle.
  // here we explicitly create one with a blank password
  {
    bts::wallet::wallet_ptr wallet = std::make_shared<bts::wallet::wallet>();
    wallet->set_data_directory(numbered_config_dir);
    fc::path wallet_data_filename = wallet->get_wallet_file();
    wallet->create(wallet_data_filename, "", WALLET_PASPHRASE);
  }

  options.push_back("--data-dir");
  options.push_back(numbered_config_dir.string());

  options.push_back("--server");
  options.push_back("--rpcuser=" RPC_USERNAME);
  options.push_back("--rpcpassword=" RPC_PASSWORD);
  options.push_back("--rpcport");
  options.push_back(boost::lexical_cast<std::string>(rpc_port));
  options.push_back("--trustee-address");
  options.push_back(bts::blockchain::address(trustee_key.get_public_key()));
  if (act_as_trustee)
  {
    options.push_back("--trustee-private-key");
    options.push_back(trustee_key.get_secret());
  }

  bts_client_process->exec(bts_xt_client_test_config::bts_client_exe, options, numbered_config_dir);

#if 0
  std::shared_ptr<std::ofstream> stdouterrfile = std::make_shared<std::ofstream>((numbered_config_dir / "stdouterr.txt").string().c_str());
  fc::buffered_istream_ptr out_stream = bts_client_process->out_stream();
  fc::buffered_istream_ptr err_stream = bts_client_process->err_stream();
  server_process_info->stdout_reader_done = fc::async([out_stream,stdouterrfile]()
    {
      char buf[1024];
      for (;;)
      {
        size_t bytes_read = out_stream->readsome(buf, sizeof(buf));
        if (!bytes_read)
          break;
        stdouterrfile->write(buf, bytes_read);
      }
    });
  server_process_info->stderr_reader_done = fc::async([err_stream,stdouterrfile]()
    {
      char buf[1024];
      for (;;)
      {
        size_t bytes_read = err_stream->readsome(buf, sizeof(buf));
        if (!bytes_read)
          break;
        stdouterrfile->write(buf, bytes_read);
      }
    });
#endif
  return bts_client_process;
}

struct test_client
{
  fc::ecc::private_key private_key;
  fc::process_ptr process;
  uint16_t rpc_port;
  bts::rpc::rpc_client_ptr rpc_client;
};

BOOST_AUTO_TEST_CASE(transfer_test)
{
  std::vector<test_client> client_processes;

  const uint32_t test_process_count = 10;

  client_processes.resize(test_process_count);

  // generate a genesis block giving 100bts to each account
  BOOST_TEST_MESSAGE("Generating keys for " << test_process_count << " clients");
  genesis_block_config genesis_block;
  for (int i = 0; i < test_process_count; ++i)
  {
    client_processes[i].private_key = fc::ecc::private_key::generate();
    genesis_block.balances.push_back(std::make_pair(bts::blockchain::pts_address(client_processes[i].private_key.get_public_key()), INITIAL_BALANCE));
    //client_processes[i].bts_address = bts::blockchain::address(client_processes[i].private_key.get_public_key());
  }

  BOOST_TEST_MESSAGE("Generating trustee keypair");
  fc::ecc::private_key trustee_key = fc::ecc::private_key::generate();

  BOOST_TEST_MESSAGE("Launching bts_xt_server processe");
  bts_server_process_info_ptr bts_server_process = launch_bts_server_process(genesis_block, trustee_key);

  BOOST_TEST_MESSAGE("Launching " << test_process_count << " bts_xt_client processes");
  for (int i = 0; i < test_process_count; ++i)
  {
    client_processes[i].rpc_port = bts_xt_client_test_config::base_rpc_port + i;
    client_processes[i].process = launch_bts_client_process(i, client_processes[i].rpc_port,
                                                            trustee_key, i == 0);
  }

  BOOST_TEST_MESSAGE("Establishing JSON-RPC connections to all processes");
  for (int i = 0; i < test_process_count; ++i)
  {
    client_processes[i].rpc_client = std::make_shared<bts::rpc::rpc_client>();
    client_processes[i].rpc_client->connect_to(fc::ip::endpoint(fc::ip::address("127.0.0.1"), client_processes[i].rpc_port));
  }

  BOOST_TEST_MESSAGE("Logging in to JSON-RPC connections");
  for (int i = 0; i < test_process_count; ++i)
    client_processes[i].rpc_client->login(RPC_USERNAME, RPC_PASSWORD);

  BOOST_TEST_MESSAGE("Verifying all clients have zero balance");
  for (int i = 0; i < test_process_count; ++i)
  {
    bts::blockchain::asset balance = client_processes[i].rpc_client->getbalance(0);
    BOOST_CHECK(balance == bts::blockchain::asset());
  }

  BOOST_TEST_MESSAGE("Testing unlocking wallets");
  for (int i = 0; i < test_process_count; ++i)
  {
    BOOST_CHECK(!client_processes[i].rpc_client->walletpassphrase("this is not the correct wallet passphrase"));
    BOOST_CHECK(client_processes[i].rpc_client->walletpassphrase(WALLET_PASPHRASE));
  }

  BOOST_TEST_MESSAGE("Testing receive address generation");
  for (int i = 0; i < test_process_count; ++i)
  {
    std::unordered_map<bts::blockchain::address, std::string> initial_addresses = client_processes[i].rpc_client->listrecvaddresses();
    BOOST_CHECK(initial_addresses.empty());
    std::string accountName("address_test_account");
    bts::blockchain::address new_address = client_processes[i].rpc_client->getnewaddress(accountName);
    BOOST_CHECK(initial_addresses.find(new_address) == initial_addresses.end());
    std::unordered_map<bts::blockchain::address, std::string> final_addresses = client_processes[i].rpc_client->listrecvaddresses();
    BOOST_CHECK(final_addresses.size() == initial_addresses.size() + 1);
    for (auto value : initial_addresses)
    {
      BOOST_CHECK(final_addresses.find(value.first) != final_addresses.end());
    }
    BOOST_CHECK(final_addresses.find(new_address) != final_addresses.end());
    BOOST_CHECK(final_addresses[new_address] == accountName);
  }

  BOOST_TEST_MESSAGE("Importing initial keys and verifying initial balances");
  for (int i = 0; i < test_process_count; ++i)
  {
    client_processes[i].rpc_client->import_private_key(client_processes[i].private_key.get_secret());
    client_processes[i].rpc_client->rescan(0);
    BOOST_CHECK(client_processes[i].rpc_client->getbalance(0) == INITIAL_BALANCE);
  }

  BOOST_TEST_MESSAGE("Sending 1 million BTS to the next client in the list");
  for (int i = 0; i < test_process_count; ++i)
  {
    uint32_t next_client_index = (i + 1) % test_process_count;    
    bts::blockchain::address destination_address = client_processes[next_client_index].rpc_client->getnewaddress("circle_test");
    bts::blockchain::asset destination_initial_balance = client_processes[next_client_index].rpc_client->getbalance(0);
    bts::blockchain::asset source_initial_balance = client_processes[i].rpc_client->getbalance(0);
    const uint32_t amount_to_transfer = 1000000;
    client_processes[i].rpc_client->transfer(amount_to_transfer, destination_address);
    fc::time_point transfer_time = fc::time_point::now();
    for (;;)
    {
      fc::usleep(fc::milliseconds(500));
      if (client_processes[next_client_index].rpc_client->getbalance(0) == destination_initial_balance + amount_to_transfer)
      {
        BOOST_TEST_MESSAGE("Client " << next_client_index << " received 1MBTS from client " << i);
        break;
      }
      if (fc::time_point::now() > transfer_time + fc::seconds(35))
        BOOST_FAIL("Client did not report receiving the transfer within 35 seconds");
    }
  }

  try{
    BOOST_TEST_MESSAGE("sleeping");
    fc::usleep( fc::seconds(10) );
  } catch ( const fc::exception& e )
  {
    wlog( "${e}", ("e",e.to_detail_string() ) );
    throw;
  }
}
