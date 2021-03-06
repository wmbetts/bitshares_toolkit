#include <bts/cli/cli.hpp>
#include <fc/thread/thread.hpp>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <fc/reflect/variant.hpp>
#include <fc/io/json.hpp>

#include <fc/log/logger.hpp>

#ifndef WIN32
#include <readline/readline.h>
#include <readline/history.h>
#endif //!WIN32

namespace bts { namespace cli {

   namespace detail
   {
      class cli_impl
      {
         public:
            client_ptr       _client;
            bts::cli::cli*   _self;
            fc::thread*      _main_thread;
            fc::thread       _cin_thread;
            fc::future<void> _cin_complete;

            std::string get_line( const std::string& prompt = ">>> " )
            {
                  std::string line;
                  #ifndef WIN32
                     char* line_read = nullptr;
                     line_read = readline(prompt.c_str());
                     if(line_read && *line_read)
                         add_history(line_read);
                     if( line_read == nullptr )
                        FC_THROW_EXCEPTION( eof_exception, "" );
                     line = line_read;
                     free(line_read);
                  #else
                     std::cout<<prompt;
                     std::getline( std::cin, line );
                  #endif ///WIN32
                  return line;
            }
            bool check_unlock()
            {
               if( _client->get_wallet()->is_locked() )
               {
                  auto password = _self->get_line( "key password: " );
                  try {
                     _client->get_wallet()->unlock_wallet( password );
                  }
                  catch ( ... )
                  {
                     std::cout << "Invalid Password\n";
                     return false;
                  }
               }
               return true;
            }

            void process_commands()
            {
                  _self->process_command( "login", "" );
                  auto line = _self->get_line();
                  while( std::cin.good() )
                  {
                     std::stringstream ss(line);
                     std::string command, args;

                     ss >> command;
                     std::getline( ss, args );

                     try {
                       _self->process_command( command, args );
                     }
                     catch ( const fc::canceled_exception& c )
                     {
                        return;
                     }
                     catch ( const fc::exception& e )
                     {
                        std::cout << e.to_detail_string() << "\n";
                     }
                     line = _self->get_line();
                  }
            }
      };
   }

   cli::cli( const client_ptr& c )
   :my( new detail::cli_impl() )
   {
      my->_client      = c;
      my->_self        = this;
      my->_main_thread = &fc::thread::current();


      my->_cin_complete = fc::async( [=](){ my->process_commands(); } );
   }

   cli::~cli()
   {
      try {
        wait();
      }
      catch ( const fc::exception& e )
      {
         wlog( "${e}", ("e",e.to_detail_string()) );
      }
   }

   void cli::print_help()
   {
      std::cout<<"Commands\n";
      std::cout<<"-------------------------------------------------------------\n";
      std::cout<<"help - print available commands\n";
      std::cout<<"unlock - asks for password to decrypt private keys \n";
      std::cout<<"listrecvaddresses\n";
      std::cout<<"getnewaddress [ACCOUNT] \n";
      std::cout<<"sendtoaddress ADDRESS AMOUNT [MEMO] \n";
      std::cout<<"getbalance [ACCOUNT] [MIN_CONF] \n";
      std::cout<<"listtransactions [COUNT]\n";
      std::cout<<"rescan [BLOCK_NUM=0]\n";
      std::cout<<"import_bitcoin_wallet WALLET_DAT\n";
      std::cout<<"import_private_key    HEX_PRIV_KEY\n";
      std::cout<<"listunspent\n";
      std::cout<<"quit - exit cleanly\n";
      std::cout<<"-------------------------------------------------------------\n";
   } // print_help
   
   void cli::confirm_and_broadcast(signed_transaction& tx)
   {
       auto wallet = client()->get_wallet();
       auto db = client()->get_chain();
       char response;

       std::cout << "About to broadcast transaction:\n\n";
       std::cout << wallet->get_tx_info_string( *db, tx ) << "\n";
       std::cout << "Send this transaction? (Y/n)\n";
       std::cin >> response;

       if (response == 'Y')
       {
           client()->broadcast_transaction( tx );
           std::cout << "Transaction sent.\n";
       }
       else
       {
           std::cout << "Transaction canceled.\n";
       }
   }

   void cli::process_command( const std::string& cmd, const std::string& args )
   {
       const bts::blockchain::chain_database_ptr db = client()->get_chain();
       const bts::wallet::wallet_ptr wallet = client()->get_wallet();

       std::stringstream ss(args);

       if( cmd == "help" ) print_help();
       else if( cmd == "login" )
       {
          auto wallet_dat = wallet->get_wallet_file();
          if( fc::exists( wallet_dat ) )
          {
             try
             {
                // try to open without a password first
                wallet->open( wallet_dat, "" );
                return;
             }
             catch (fc::exception&)
             {}
             // else ask for a password
             std::cout << "Login\n";
             auto pass = get_line("password: ");
             wallet->open( wallet_dat, pass );
             std::cout << "Login Successful.\n";
             return;
          }
          else
          {
             std::cout << "Creating wallet "<< wallet_dat.generic_string() << "\n";
             std::cout << "You will be asked to provide two passwords, the first password \n";
             std::cout << "encrypts the entire contents of your wallet on disk.  The second\n ";
             std::cout << "passowrd will only encrypt your private keys.\n\n";

             std::cout << "Please set a password for encrypting your wallet: \n";
             std::string pass1, pass2;
             pass1  = get_line("password: ");
             while( pass1 != pass2 )
             {
                pass2 = get_line("password (again): ");
                if( pass2 != pass1 )
                {
                  std::cout << "Your passwords did not match, please try again.\n";
                  pass2 = std::string();
                  pass1 = get_line("password: ");
                }
             }
             if( pass1 == std::string() )
             {
                std::cout << "No password provided, your wallet will be stored unencrypted.\n";
             }

             std::cout << "Please set a password for encrypting your private keys: \n";
             std::string keypass1, keypass2;
             bool retry = false;
             keypass1  = get_line("key password: ");
             while( keypass1 != keypass2 )
             {
                if( keypass1.size() > 8 )
                   keypass2 = get_line("key password (again): ");
                else
                {
                   std::cout << "Your key password must be more than 8 characters.\n";
                   retry = true;
                }
                if( keypass2 != keypass1 )
                {
                   std::cout << "Your passwords did not match.\n";
                   retry = true;
                }
                if( retry )
                {
                  retry = false;
                  std::cout << "Please try again.\n";
                  keypass2 = std::string();
                  keypass1 = get_line("password: ");
                }
             }
             if( keypass1 == std::string() )
             {
                std::cout << "No password provided, your wallet will be stored unencrypted.\n";
             }

             wallet->create( wallet_dat, pass1, keypass1 );
             std::cout << "Wallet created.\n";
          }
       }
       else if( cmd == "getnewaddress" )
       {
          if( my->check_unlock() )
          {
             std::string account;
             ss >> account;

             auto addr = wallet->new_recv_address( account );
             std::cout << std::string( addr ) << "\n";
          }
       }
       else if( cmd == "listunspent" )
       {
          wallet->dump_utxo_set();
       }
       else if( cmd == "sendtoaddress" )
       {
          if( my->check_unlock() )
          {
             std::string addr;
             double      amount;
             std::string memo;
             ss >> addr >> amount;
             std::cout << "memo: ";
             std::getline( ss, memo );

             auto trx = wallet->transfer( asset( amount ), address(addr), memo );
             confirm_and_broadcast( trx );
          }
       }
       else if( cmd == "listrecvaddresses" )
       {
           auto addrs = wallet->get_recv_addresses();
           for( auto addr : addrs )
              std::cout << std::setw( 30 ) << std::left << std::string(addr.first) << " : " << addr.second << "\n";
       }
       else if( cmd == "import" )
       {

       }
       else if( cmd == "listtransactions" )
       {
          uint32_t count = 0;
          ss >> count;
          list_transactions( count );
       }
       else if( cmd == "rescan" )
       {
          uint32_t block_num = 0;
          ss >> block_num;
          wallet->scan_chain( *db, block_num, [](uint32_t cur, uint32_t last, uint32_t trx, uint32_t last_trx)
                                                 {
                                                     std::cout << "scanning transaction " <<  cur << "." << trx <<"  of " << last << "." << last_trx << "         \r";
                                                 });
          std::cout << "\ndone scanning block chain\n";
       }
       else if( cmd == "export" )
       {

       }
       else if( cmd == "import_bitcoin_wallet" )
       {
          if( my->check_unlock() )
          {
             // TODO: Enforce # of arguments
             std::string wallet_dat;
             ss >> wallet_dat;

             auto password  = get_line("bitcoin wallet password: ");
             wallet->import_bitcoin_wallet( wallet_dat, password );
             wallet->save();
          }
       }
       else if( cmd == "import_private_key" )
       {
          if( my->check_unlock() )
          {
             // TODO: Enforce # of arguments
             std::string key_str;
             ss >> key_str;

             fc::sha256 hash( key_str );
             fc::ecc::private_key privkey = fc::ecc::private_key::regenerate( hash );

             wallet->import_key( privkey );
             wallet->save();
          }
       }
       else if( cmd == "getbalance" )
       {
          uint32_t minconf = 0;
          ss >> minconf;
          get_balance(minconf);
       }
       else if( cmd == "quit" )
       {
          FC_THROW_EXCEPTION( canceled_exception, "quit command issued" );
       }
       else
       {
          std::cout<<"Unknown command '"<<cmd<<"'\n\n";
          print_help();
       }
   }
   void cli::list_transactions( uint32_t count )
   {
       /* dump the transactions from the wallet, which needs the chain db */
       client()->get_wallet()->dump_txs(*(client()->get_chain()), count);
   }
   void cli::get_balance( uint32_t min_conf, uint16_t unit )
   {
       asset bal = client()->get_wallet()->get_balance(unit);
       std::cout << std::string( bal ) << "\n";
   }

   void cli::wait()
   {
       my->_cin_complete.wait();
   }

   std::string cli::get_line( const std::string& prompt )
   {
       return my->_cin_thread.async( [=](){ return my->get_line( prompt ); } ).wait();
   }

   client_ptr cli::client()
   {
       return my->_client;
   }

   bool cli::check_unlock()
   {
       return my->check_unlock();
   }

} } // bts::cli
