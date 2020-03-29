/*
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srsue/hdr/stack/ue_stack_lte.h"
#include "srslte/common/logmap.h"
#include "srslte/srslte.h"
#include <algorithm>
#include <chrono>
#include <numeric>

using namespace srslte;

namespace srsue {

ue_stack_lte::ue_stack_lte() :
  timers(64),
  running(false),
  args(),
  logger(nullptr),
  usim(nullptr),
  phy(nullptr),
  rlc("RLC"),
  mac("MAC "),
  rrc(this),
  pdcp(&timers, "PDCP"),
  nas(this),
  thread("STACK"),
  pending_tasks(512),
  background_tasks(2),
  tti_tprof("tti_tprof", "STCK", TTI_STAT_PERIOD)
{
  ue_queue_id         = pending_tasks.add_queue();
  sync_queue_id       = pending_tasks.add_queue();
  gw_queue_id         = pending_tasks.add_queue();
  mac_queue_id        = pending_tasks.add_queue();
  background_queue_id = pending_tasks.add_queue();

  background_tasks.start();
}

ue_stack_lte::~ue_stack_lte()
{
  stop();
}

std::string ue_stack_lte::get_type()
{
  return "lte";
}

int ue_stack_lte::init(const stack_args_t&      args_,
                       srslte::logger*          logger_,
                       phy_interface_stack_lte* phy_,
                       gw_interface_stack*      gw_)
{
  phy = phy_;
  gw  = gw_;

  if (init(args_, logger_)) {
    return SRSLTE_ERROR;
  }

  return SRSLTE_SUCCESS;
}

int ue_stack_lte::init(const stack_args_t& args_, srslte::logger* logger_)
{
  args   = args_;
  logger = logger_;

  // init own log
  stack_log->set_level(srslte::LOG_LEVEL_INFO);
  pool_log->set_level(srslte::LOG_LEVEL_WARNING);
  byte_buffer_pool::get_instance()->set_log(pool_log.get());

  // init layer logs
  srslte::logmap::register_log(std::unique_ptr<srslte::log>{new srslte::log_filter{"MAC", logger, true}});
  mac_log->set_level(args.log.mac_level);
  mac_log->set_hex_limit(args.log.mac_hex_limit);
  rlc_log->set_level(args.log.rlc_level);
  rlc_log->set_hex_limit(args.log.rlc_hex_limit);
  pdcp_log->set_level(args.log.pdcp_level);
  pdcp_log->set_hex_limit(args.log.pdcp_hex_limit);
  rrc_log->set_level(args.log.rrc_level);
  rrc_log->set_hex_limit(args.log.rrc_hex_limit);
  usim_log->set_level(args.log.usim_level);
  usim_log->set_hex_limit(args.log.usim_hex_limit);
  nas_log->set_level(args.log.nas_level);
  nas_log->set_hex_limit(args.log.nas_hex_limit);

  // Set up pcap
  if (args.pcap.enable) {
    mac_pcap.open(args.pcap.filename.c_str());
    mac.start_pcap(&mac_pcap);
  }
  if (args.pcap.nas_enable) {
    nas_pcap.open(args.pcap.nas_filename.c_str());
    nas.start_pcap(&nas_pcap);
  }

  // Init USIM first to allow early exit in case reader couldn't be found
  usim = usim_base::get_instance(&args.usim, usim_log.get());
  if (usim->init(&args.usim)) {
    usim_log->console("Failed to initialize USIM.\n");
    return SRSLTE_ERROR;
  }

  mac.init(phy, &rlc, &rrc, this);
  rlc.init(&pdcp, &rrc, &timers, 0 /* RB_ID_SRB0 */);
  pdcp.init(&rlc, &rrc, gw);
  nas.init(usim.get(), &rrc, gw, args.nas);
  rrc.init(phy, &mac, &rlc, &pdcp, &nas, usim.get(), gw, args.rrc);

  running = true;
  start(STACK_MAIN_THREAD_PRIO);

  return SRSLTE_SUCCESS;
}

void ue_stack_lte::stop()
{
  if (running) {
    pending_tasks.try_push(ue_queue_id, [this]() { stop_impl(); });
    wait_thread_finish();
  }
}

void ue_stack_lte::stop_impl()
{
  running = false;

  usim->stop();
  nas.stop();
  rrc.stop();

  rlc.stop();
  pdcp.stop();
  mac.stop();

  if (args.pcap.enable) {
    mac_pcap.close();
  }
  if (args.pcap.nas_enable) {
    nas_pcap.close();
  }
}

bool ue_stack_lte::switch_on()
{
  if (running) {
    pending_tasks.try_push(ue_queue_id,
                           [this]() { nas.start_attach_proc(nullptr, srslte::establishment_cause_t::mo_sig); });
    return true;
  }
  return false;
}

bool ue_stack_lte::switch_off()
{
  // generate detach request with switch-off flag
  nas.detach_request(true);

  // wait for max. 5s for it to be sent (according to TS 24.301 Sec 25.5.2.2)
  const uint32_t RB_ID_SRB1 = 1;
  int            cnt = 0, timeout = 5000;

  while (rlc.has_data(RB_ID_SRB1) && ++cnt <= timeout) {
    usleep(1000);
  }
  bool detach_sent = true;
  if (rlc.has_data(RB_ID_SRB1)) {
    logmap::get("NAS ")->warning("Detach couldn't be sent after %ds.\n", timeout);
    detach_sent = false;
  }

  return detach_sent;
}

bool ue_stack_lte::enable_data()
{
  // perform attach request
  stack_log->console("Turning off airplane mode.\n");
  return switch_on();
}

bool ue_stack_lte::disable_data()
{
  // generate detach request
  stack_log->console("Turning on airplane mode.\n");
  return nas.detach_request(false);
}

bool ue_stack_lte::get_metrics(stack_metrics_t* metrics)
{
  // use stack thread to query metrics
  pending_tasks.try_push(ue_queue_id, [this]() {
    stack_metrics_t metrics{};
    mac.get_metrics(metrics.mac);
    rlc.get_metrics(metrics.rlc);
    nas.get_metrics(&metrics.nas);
    rrc.get_metrics(metrics.rrc);
    pending_stack_metrics.push(metrics);
  });
  // wait for result
  *metrics = pending_stack_metrics.wait_pop();
  return (metrics->nas.state == EMM_STATE_REGISTERED && metrics->rrc.state == RRC_STATE_CONNECTED);
}

void ue_stack_lte::run_thread()
{
  while (running) {
    srslte::move_task_t task{};
    if (pending_tasks.wait_pop(&task) >= 0) {
      task();
    }
  }
}

/***********************************************************************************************************************
 *                                                Stack Interfaces
 **********************************************************************************************************************/

/********************
 *   GW Interface
 *******************/

/**
 * Push GW SDU to stack
 * @param lcid
 * @param sdu
 * @param blocking
 */
void ue_stack_lte::write_sdu(uint32_t lcid, srslte::unique_byte_buffer_t sdu, bool blocking)
{
  auto task = [this, lcid, blocking](srslte::unique_byte_buffer_t& sdu) {
    pdcp.write_sdu(lcid, std::move(sdu), blocking);
  };
  bool ret = pending_tasks.try_push(gw_queue_id, std::bind(task, std::move(sdu))).first;
  if (not ret) {
    pdcp_log->warning("GW SDU with lcid=%d was discarded.\n", lcid);
  }
}

/********************
 *  SYNC Interface
 *******************/

/**
 * Sync thread signal that it is in sync
 */
void ue_stack_lte::in_sync()
{
  pending_tasks.push(sync_queue_id, [this]() { rrc.in_sync(); });
}

void ue_stack_lte::out_of_sync()
{
  pending_tasks.push(sync_queue_id, [this]() { rrc.out_of_sync(); });
}

void ue_stack_lte::run_tti(uint32_t tti, uint32_t tti_jump)
{
  pending_tasks.push(sync_queue_id, [this, tti, tti_jump]() { run_tti_impl(tti, tti_jump); });
}

void ue_stack_lte::run_tti_impl(uint32_t tti, uint32_t tti_jump)
{
  if (args.have_tti_time_stats) {
    tti_tprof.start();
  }
  current_tti = tti_point{tti};

  // perform tasks for the received TTI range
  for (uint32_t i = 0; i < tti_jump; ++i) {
    uint32_t next_tti = TTI_SUB(tti, (tti_jump - i - 1));
    mac.run_tti(next_tti);
    timers.step_all();
  }
  rrc.run_tti();
  nas.run_tti();

  if (args.have_tti_time_stats) {
    std::chrono::nanoseconds dur = tti_tprof.stop();
    if (dur > TTI_WARN_THRESHOLD_MS) {
      mac_log->warning("%s: detected long duration=%ld ms\n",
                       "proc_time",
                       std::chrono::duration_cast<std::chrono::milliseconds>(dur).count());
    }
  }

  // print warning if PHY pushes new TTI messages faster than we process them
  if (pending_tasks.size(sync_queue_id) > SYNC_QUEUE_WARN_THRESHOLD) {
    stack_log->warning("Detected slow task processing (sync_queue_len=%zd).\n", pending_tasks.size(sync_queue_id));
  }
}

/***************************
 * Task Handling Interface
 **************************/

void ue_stack_lte::enqueue_background_task(std::function<void(uint32_t)> f)
{
  background_tasks.push_task(std::move(f));
}

void ue_stack_lte::notify_background_task_result(srslte::move_task_t task)
{
  // run the notification in the stack thread
  pending_tasks.push(background_queue_id, std::move(task));
}

void ue_stack_lte::defer_callback(uint32_t duration_ms, std::function<void()> func)
{
  timers.defer_callback(duration_ms, func);
}

/********************
 *  RRC Interface
 *******************/

void ue_stack_lte::start_cell_search()
{
  background_tasks.push_task([this](uint32_t worker_id) {
    phy_interface_rrc_lte::phy_cell_t        found_cell;
    phy_interface_rrc_lte::cell_search_ret_t ret = phy->cell_search(&found_cell);
    // notify back RRC
    pending_tasks.push(background_queue_id, [this, found_cell, ret]() { rrc.cell_search_completed(ret, found_cell); });
  });
}

void ue_stack_lte::start_cell_select(const phy_interface_rrc_lte::phy_cell_t* phy_cell)
{
  background_tasks.push_task([this, phy_cell](uint32_t worker_id) {
    bool ret = phy->cell_select(phy_cell);
    // notify back RRC
    pending_tasks.push(background_queue_id, [this, ret]() { rrc.cell_select_completed(ret); });
  });
}

} // namespace srsue
