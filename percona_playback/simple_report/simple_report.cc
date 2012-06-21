/* BEGIN LICENSE
 * Copyright (C) 2011-2012 Percona Inc.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 * END LICENSE */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "boost/foreach.hpp"
#include <boost/program_options.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "tbb/atomic.h"
#include "tbb/concurrent_unordered_map.h"
#include <percona_playback/plugin.h>
#include <percona_playback/query_result.h>

class SimpleReportPlugin : public percona_playback::ReportPlugin
{
private:
  tbb::atomic<uint64_t> nr_expected_rows_sent;
  tbb::atomic<uint64_t> nr_actual_rows_sent;
  tbb::atomic<uint64_t> nr_queries_rows_differ;
  tbb::atomic<uint64_t> nr_queries_executed;
  tbb::atomic<uint64_t> nr_error_queries;
  tbb::atomic<uint64_t> total_execution_time_ms;
  tbb::atomic<uint64_t> expected_total_execution_time_ms;
  tbb::atomic<uint64_t> nr_quicker_queries;
  tbb::atomic<uint64_t> nr_slower_queries;

  typedef tbb::concurrent_unordered_map<uint64_t, tbb::atomic<uint64_t> > ConnectionQueryCountMap;
  typedef std::pair<uint64_t, tbb::atomic<uint64_t> > ConnectionQueryCountPair;
  typedef std::map<uint64_t, uint64_t> SortedConnectionQueryCountMap;
  typedef std::pair<uint64_t, uint64_t> SortedConnectionQueryCountPair;

  ConnectionQueryCountMap connection_query_counts;

  bool show_connection_query_count;

public:
  SimpleReportPlugin(std::string _name) : ReportPlugin(_name)
  {
    nr_queries_executed= 0;
    nr_expected_rows_sent= 0;
    nr_actual_rows_sent= 0;
    nr_queries_rows_differ= 0;
    nr_error_queries= 0;
    show_connection_query_count= false;
    total_execution_time_ms= 0;
    expected_total_execution_time_ms= 0;
    nr_quicker_queries= 0;
    nr_slower_queries= 0;
  }

  virtual boost::program_options::options_description* getProgramOptions() {
    namespace po= boost::program_options;

    static po::options_description simple_report_options("Simple Report Options");
    simple_report_options.add_options()
    ("show-per-connection-query-count",
     po::value<bool>(&show_connection_query_count)->default_value(false)->zero_tokens(),
     "For each connection, display the number of queries executed.")
    ;

    return &simple_report_options;
  }

  virtual void query_execution(const uint64_t thread_id,
			       const std::string &query,
			       const QueryResult &expected,
			       const QueryResult &actual)
  {
    if (actual.getError())
    {
      fprintf(stderr,"Error query: %s\n", query.c_str());
      nr_error_queries++;
    }

    {
      tbb::atomic<uint64_t> zero;
      zero= 0;

      std::pair<ConnectionQueryCountMap::iterator, bool> it_pair=
	connection_query_counts.insert(ConnectionQueryCountPair(thread_id, zero));

      (*(it_pair.first)).second++;
    }


    if (actual.getRowsSent() != expected.getRowsSent())
    {
      nr_queries_rows_differ++;
      fprintf(stderr, "Connection %"PRIu64" Rows Sent: %"PRIu64 " != expected %"PRIu64 " for query: %s\n", thread_id, actual.getRowsSent(), expected.getRowsSent(), query.c_str());
    }

    nr_queries_executed++;
    nr_expected_rows_sent.fetch_and_add(expected.getRowsSent());
    nr_actual_rows_sent.fetch_and_add(actual.getRowsSent());

    total_execution_time_ms.fetch_and_add(actual.getDuration().total_microseconds());

    if (expected.getDuration().total_microseconds())
    {
      expected_total_execution_time_ms.fetch_and_add(expected.getDuration().total_microseconds());
      if (actual.getDuration().total_microseconds() < expected.getDuration().total_microseconds())
        nr_quicker_queries++;
      else
        nr_slower_queries++;
    }
  }

  virtual void print_report()
  {
    printf("Report\n");
    printf("------\n");
    printf("Executed %" PRIu64 " queries\n", uint64_t(nr_queries_executed));

    boost::posix_time::time_duration total_duration= boost::posix_time::microseconds(total_execution_time_ms);
    boost::posix_time::time_duration expected_duration= boost::posix_time::microseconds(expected_total_execution_time_ms);
    printf("Spent %s executing queries versus an expected %s time.\n",
           boost::posix_time::to_simple_string(total_duration).c_str(),
           boost::posix_time::to_simple_string(expected_duration).c_str()
           );
    printf("%"PRIu64 " queries were quicker than expected, %"PRIu64" were slower\n",
           uint64_t(nr_quicker_queries),
           uint64_t(nr_slower_queries));

    printf("A total of %" PRIu64 " queries had errors.\n",
	   uint64_t(nr_error_queries));
    printf("Expected %" PRIu64 " rows, got %" PRIu64 " (a difference of %" PRId64 ")\n",
	   uint64_t(nr_expected_rows_sent),
	   uint64_t(nr_actual_rows_sent),
	   labs(int64_t(nr_expected_rows_sent) - int64_t(nr_actual_rows_sent))
	   );
    printf("Number of queries where number of rows differed: %" PRIu64 ".\n",
	   uint64_t(nr_queries_rows_differ));

    SortedConnectionQueryCountMap sorted_conn_count;
    uint64_t total_queries= 0;

    BOOST_FOREACH(const ConnectionQueryCountPair conn_count,
		  connection_query_counts)
    {
      sorted_conn_count.insert(SortedConnectionQueryCountPair(conn_count.first, uint64_t(conn_count.second)));

      total_queries+= uint64_t(conn_count.second);
    }

    double avg_queries= (double)total_queries / (double)connection_query_counts.size();

    printf("\n");
    printf("Average of %.2f queries per connection (%"PRIu64 " connections).\n", avg_queries, connection_query_counts.size());
    printf("\n");

    if (show_connection_query_count)
    {
      printf("Per Thread results\n");
      printf("------------------\n");
      printf("Conn Id\t\tQueries\n");
      BOOST_FOREACH(const SortedConnectionQueryCountPair conn_count,
		    sorted_conn_count)
      {
	printf("%"PRIu64 "\t\t%"PRIu64 "\n",
	       conn_count.first,
	       conn_count.second);
      }

      printf("\n");
    }

  }

};

static void init_plugin(percona_playback::PluginRegistry &r)
{
  r.add("simple_report", new SimpleReportPlugin("simple_report"));
}

PERCONA_PLAYBACK_PLUGIN(init_plugin);