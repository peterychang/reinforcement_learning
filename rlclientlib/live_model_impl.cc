#include "live_model_impl.h"

#include "api_status.h"
#include "configuration.h"
#include "constants.h"
#include "err_constants.h"
#include "error_callback_fn.h"
#include "factory_resolver.h"
#include "internal_constants.h"
#include "logger/preamble_sender.h"
#include "ranking_response.h"
#include "sampling.h"
#include "sender.h"
#include "trace_logger.h"
#include "utility/context_helper.h"
#include "vw/common/hash.h"
#include "vw/explore/explore.h"
#include "vw_model/safe_vw.h"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cmath>
#include <cstring>

// Some namespace changes for more concise code
namespace e = exploration;
using namespace std;

namespace reinforcement_learning
{
// Some namespace changes for more concise code
namespace m = model_management;
namespace u = utility;
namespace l = logger;

int check_null_or_empty(const char* arg1, string_view arg2, i_trace* trace, api_status* status);
int check_null_or_empty(const char* arg1, i_trace* trace, api_status* status);
int check_null_or_empty(string_view arg1, i_trace* trace, api_status* status);
int reset_action_order(ranking_response& response);
void autogenerate_missing_uuids(
    const std::map<size_t, std::string>& found_ids, std::vector<std::string>& complete_ids, uint64_t seed_shift);
int reset_chosen_action_multi_slot(
    multi_slot_response& response, const std::vector<int>& baseline_actions = std::vector<int>());
int reset_chosen_action_multi_slot(
    multi_slot_response_detailed& response, const std::vector<int>& baseline_actions = std::vector<int>());

void default_error_callback(const api_status& status, void* watchdog_context)
{
  auto* watchdog = static_cast<utility::watchdog*>(watchdog_context);
  watchdog->set_unhandled_background_error(true);
}

int live_model_impl::init(api_status* status)
{
  RETURN_IF_FAIL(init_trace(status));
  RETURN_IF_FAIL(init_model(status));
  RETURN_IF_FAIL(init_model_mgmt(status));
  RETURN_IF_FAIL(init_loggers(status));

  if (_protocol_version == 1)
  {
    if (_configuration.get_bool("interaction", name::USE_COMPRESSION, false) ||
        _configuration.get_bool("interaction", name::USE_DEDUP, false) ||
        _configuration.get_bool("observation", name::USE_COMPRESSION, false))
    {
      RETURN_ERROR_LS(_trace_logger.get(), status, content_encoding_error);
    }
  }

  _initial_epsilon = _configuration.get_float(name::INITIAL_EPSILON, 0.2f);
  const char* app_id = _configuration.get(name::APP_ID, "");
  _seed_shift = VW::uniform_hash(app_id, strlen(app_id), 0);

  return error_code::success;
}

int live_model_impl::choose_rank(
    const char* event_id, string_view context, unsigned int flags, ranking_response& response, api_status* status)
{
  response.clear();
  // clear previous errors if any
  api_status::try_clear(status);

  // check arguments
  RETURN_IF_FAIL(check_null_or_empty(event_id, context, _trace_logger.get(), status));

  // The seed used is composed of uniform_hash(app_id) + uniform_hash(event_id)
  const uint64_t seed = VW::uniform_hash(event_id, strlen(event_id), 0) + _seed_shift;

  std::vector<int> action_ids;
  std::vector<float> action_pdf;
  std::string model_version;

  _model->choose_rank(event_id, seed, context, action_ids, action_pdf, model_version, status);

  RETURN_IF_FAIL(sample_and_populate_response(
      seed, action_ids, action_pdf, std::move(model_version), response, _trace_logger.get(), status));

  response.set_event_id(event_id);

  if (_learning_mode == LOGGINGONLY)
  {
    // Reset the ranked action order before logging
    RETURN_IF_FAIL(reset_action_order(response));
  }

  RETURN_IF_FAIL(_interaction_logger->log(context, flags, response, status, _learning_mode));

  if (_learning_mode == APPRENTICE)
  {
    // Reset the ranked action order after logging
    RETURN_IF_FAIL(reset_action_order(response));
  }

  // Check watchdog for any background errors. Do this at the end of function so that the work is still done.
  if (_watchdog.has_background_error_been_reported())
  {
    RETURN_ERROR_LS(_trace_logger.get(), status, unhandled_background_error_occurred);
  }

  return error_code::success;
}

// here the event_id is auto-generated
int live_model_impl::choose_rank(
    string_view context, unsigned int flags, ranking_response& response, api_status* status)
{
  const auto uuid = boost::uuids::to_string(boost::uuids::random_generator()());
  return choose_rank(uuid.c_str(), context, flags, response, status);
}

int live_model_impl::request_continuous_action(const char* event_id, string_view context, unsigned int flags,
    continuous_action_response& response, api_status* status)
{
  response.clear();
  // clear previous errors if any
  api_status::try_clear(status);

  RETURN_IF_FAIL(check_null_or_empty(event_id, context.data(), _trace_logger.get(), status));

  float action = NAN;
  float pdf_value = NAN;
  std::string model_version;

  RETURN_IF_FAIL(_model->choose_continuous_action(context, action, pdf_value, model_version, status));
  RETURN_IF_FAIL(populate_response(
      action, pdf_value, std::string(event_id), std::string(model_version), response, _trace_logger.get(), status));
  RETURN_IF_FAIL(_interaction_logger->log_continuous_action(context.data(), flags, response, status));

  if (_watchdog.has_background_error_been_reported())
  {
    RETURN_ERROR_LS(_trace_logger.get(), status, unhandled_background_error_occurred);
  }

  return error_code::success;
}

int live_model_impl::request_continuous_action(
    string_view context, unsigned int flags, continuous_action_response& response, api_status* status)
{
  const auto uuid = boost::uuids::to_string(boost::uuids::random_generator()());
  return request_continuous_action(uuid.c_str(), context, flags, response, status);
}

int live_model_impl::request_decision(
    string_view context_json, unsigned int flags, decision_response& resp, api_status* status)
{
  if (_learning_mode == APPRENTICE || _learning_mode == LOGGINGONLY)
  {
    // Apprentice mode and LoggingOnly mode are not supported here at this moment
    return error_code::not_supported;
  }

  resp.clear();
  // clear previous errors if any
  api_status::try_clear(status);

  // check arguments
  RETURN_IF_FAIL(check_null_or_empty(context_json, _trace_logger.get(), status));

  utility::ContextInfo context_info;
  RETURN_IF_FAIL(utility::get_context_info(context_json, context_info, _trace_logger.get(), status));

  // Ensure multi comes before slots, this is a current limitation of the parser.
  if (context_info.slots.empty() || context_info.actions.empty() ||
      context_info.slots[0].first < context_info.actions[0].first)
  {
    RETURN_ERROR_LS(_trace_logger.get(), status, json_parse_error)
        << "There must be both a _multi field and _slots, and _multi must come first.";
  }

  std::vector<std::vector<uint32_t>> actions_ids;
  std::vector<std::vector<float>> actions_pdfs;
  std::string model_version;

  size_t num_decisions = context_info.slots.size();

  std::vector<std::string> event_ids_str(num_decisions);
  std::vector<const char*> event_ids(num_decisions, nullptr);
  std::map<size_t, std::string> found_ids;
  RETURN_IF_FAIL(utility::get_event_ids(context_json, found_ids, _trace_logger.get(), status));

  autogenerate_missing_uuids(found_ids, event_ids_str, _seed_shift);

  for (int i = 0; i < event_ids.size(); i++) { event_ids[i] = event_ids_str[i].c_str(); }

  // This will behave correctly both before a model is loaded and after. Prior to a model being loaded it operates in
  // explore only mode.
  RETURN_IF_FAIL(_model->request_decision(event_ids, context_json, actions_ids, actions_pdfs, model_version, status));
  RETURN_IF_FAIL(populate_response(
      actions_ids, actions_pdfs, event_ids, std::string(model_version), resp, _trace_logger.get(), status));
  RETURN_IF_FAIL(_interaction_logger->log_decisions(
      event_ids, context_json, flags, actions_ids, actions_pdfs, model_version, status));

  // Check watchdog for any background errors. Do this at the end of function so that the work is still done.
  if (_watchdog.has_background_error_been_reported())
  {
    RETURN_ERROR_LS(_trace_logger.get(), status, unhandled_background_error_occurred);
  }

  return error_code::success;
}

int live_model_impl::request_multi_slot_decision_impl(const char* event_id, string_view context_json,
    std::vector<std::string>& slot_ids, std::vector<std::vector<uint32_t>>& action_ids,
    std::vector<std::vector<float>>& action_pdfs, std::string& model_version, api_status* status)
{
  // clear previous errors if any
  api_status::try_clear(status);

  // check arguments
  RETURN_IF_FAIL(check_null_or_empty(event_id, _trace_logger.get(), status));
  RETURN_IF_FAIL(check_null_or_empty(context_json, _trace_logger.get(), status));

  utility::ContextInfo context_info;
  RETURN_IF_FAIL(utility::get_context_info(context_json, context_info, _trace_logger.get(), status));

  // Ensure multi comes before slots, this is a current limitation of the parser.
  if (context_info.slots.empty() || context_info.actions.empty() ||
      context_info.slots[0].first < context_info.actions[0].first)
  {
    RETURN_ERROR_LS(_trace_logger.get(), status, json_parse_error)
        << "There must be both a _multi field and _slots, and _multi must come first.";
  }

  slot_ids.resize(context_info.slots.size());
  std::map<size_t, std::string> found_ids;
  RETURN_IF_FAIL(utility::get_slot_ids(context_json, context_info.slots, found_ids, _trace_logger.get(), status));
  autogenerate_missing_uuids(found_ids, slot_ids, _seed_shift);

  RETURN_IF_FAIL(_model->request_multi_slot_decision(
      event_id, slot_ids, context_json, action_ids, action_pdfs, model_version, status));
  return error_code::success;
}

int live_model_impl::request_multi_slot_decision(string_view context_json, unsigned int flags,
    multi_slot_response& resp, const std::vector<int>& baseline_actions, api_status* status)
{
  const auto uuid = boost::uuids::to_string(boost::uuids::random_generator()());
  return request_multi_slot_decision(uuid.c_str(), context_json, flags, resp, baseline_actions, status);
}

int live_model_impl::request_multi_slot_decision(const char* event_id, string_view context_json, unsigned int flags,
    multi_slot_response& resp, const std::vector<int>& baseline_actions, api_status* status)
{
  resp.clear();

  if (_learning_mode == APPRENTICE && baseline_actions.empty()) { return error_code::baseline_actions_not_defined; }

  std::vector<std::string> slot_ids;
  std::vector<std::vector<uint32_t>> action_ids;
  std::vector<std::vector<float>> action_pdfs;
  std::string model_version;

  RETURN_IF_FAIL(live_model_impl::request_multi_slot_decision_impl(
      event_id, context_json, slot_ids, action_ids, action_pdfs, model_version, status));
  RETURN_IF_FAIL(populate_multi_slot_response(action_ids, action_pdfs, std::string(event_id),
      std::string(model_version), slot_ids, resp, _trace_logger.get(), status));
  RETURN_IF_FAIL(_interaction_logger->log_decision(event_id, context_json, flags, action_ids, action_pdfs,
      model_version, slot_ids, status, baseline_actions, _learning_mode));

  if (_learning_mode == APPRENTICE || _learning_mode == LOGGINGONLY)
  {
    // Reset the chosenAction.
    // In CCB it does not make sense to reset the action order because the list of actions available for each slot is
    // not deterministic.
    RETURN_IF_FAIL(reset_chosen_action_multi_slot(resp, baseline_actions));
  }

  // Check watchdog for any background errors. Do this at the end of function so that the work is still done.
  if (_watchdog.has_background_error_been_reported())
  {
    RETURN_ERROR_LS(_trace_logger.get(), status, unhandled_background_error_occurred);
  }
  return error_code::success;
}

int live_model_impl::request_multi_slot_decision(string_view context_json, unsigned int flags,
    multi_slot_response_detailed& resp, const std::vector<int>& baseline_actions, api_status* status)
{
  const auto uuid = boost::uuids::to_string(boost::uuids::random_generator()());
  return request_multi_slot_decision(uuid.c_str(), context_json, flags, resp, baseline_actions, status);
}

int live_model_impl::request_multi_slot_decision(const char* event_id, string_view context_json, unsigned int flags,
    multi_slot_response_detailed& resp, const std::vector<int>& baseline_actions, api_status* status)
{
  resp.clear();

  if (_learning_mode == APPRENTICE && baseline_actions.empty()) { return error_code::baseline_actions_not_defined; }

  std::vector<std::string> slot_ids;
  std::vector<std::vector<uint32_t>> action_ids;
  std::vector<std::vector<float>> action_pdfs;
  std::string model_version;

  RETURN_IF_FAIL(live_model_impl::request_multi_slot_decision_impl(
      event_id, context_json, slot_ids, action_ids, action_pdfs, model_version, status));

  // set the size of buffer in response to match the number of slots
  resp.resize(slot_ids.size());

  RETURN_IF_FAIL(populate_multi_slot_response_detailed(action_ids, action_pdfs, std::string(event_id),
      std::string(model_version), slot_ids, resp, _trace_logger.get(), status));
  RETURN_IF_FAIL(_interaction_logger->log_decision(event_id, context_json, flags, action_ids, action_pdfs,
      model_version, slot_ids, status, baseline_actions, _learning_mode));

  if (_learning_mode == APPRENTICE || _learning_mode == LOGGINGONLY)
  {
    // Reset the chosenAction.
    // In CCB it does not make sense to reset the action order because the list of actions available for each slot is
    // not deterministic.
    RETURN_IF_FAIL(reset_chosen_action_multi_slot(resp, baseline_actions));
  }

  // Check watchdog for any background errors. Do this at the end of function so that the work is still done.
  if (_watchdog.has_background_error_been_reported())
  {
    RETURN_ERROR_LS(_trace_logger.get(), status, unhandled_background_error_occurred);
  }
  return error_code::success;
}

int live_model_impl::report_action_taken(const char* event_id, api_status* status)
{
  // Clear previous errors if any
  api_status::try_clear(status);
  // Send the outcome event to the backend
  return _outcome_logger->report_action_taken(event_id, status);
}

int live_model_impl::report_action_taken(const char* primary_id, const char* secondary_id, api_status* status)
{
  // Clear previous errors if any
  api_status::try_clear(status);
  // Send the outcome event to the backend
  return _outcome_logger->report_action_taken(primary_id, secondary_id, status);
}

int live_model_impl::report_outcome(const char* event_id, const char* outcome, api_status* status)
{
  // Check arguments
  RETURN_IF_FAIL(check_null_or_empty(event_id, outcome, _trace_logger.get(), status));
  return report_outcome_internal(event_id, outcome, status);
}

int live_model_impl::report_outcome(const char* event_id, float outcome, api_status* status)
{
  // Check arguments
  RETURN_IF_FAIL(check_null_or_empty(event_id, _trace_logger.get(), status));
  return report_outcome_internal(event_id, outcome, status);
}

int live_model_impl::report_outcome(const char* primary_id, int secondary_id, const char* outcome, api_status* status)
{
  // Check arguments
  RETURN_IF_FAIL(check_null_or_empty(primary_id, outcome, _trace_logger.get(), status));
  return report_outcome_internal(primary_id, secondary_id, outcome, status);
}

int live_model_impl::report_outcome(const char* primary_id, int secondary_id, float outcome, api_status* status)
{
  // Check arguments
  RETURN_IF_FAIL(check_null_or_empty(primary_id, _trace_logger.get(), status));
  return report_outcome_internal(primary_id, secondary_id, outcome, status);
}

int live_model_impl::report_outcome(
    const char* primary_id, const char* secondary_id, const char* outcome, api_status* status)
{
  // Check arguments
  RETURN_IF_FAIL(check_null_or_empty(primary_id, outcome, _trace_logger.get(), status));
  RETURN_IF_FAIL(check_null_or_empty(secondary_id, _trace_logger.get(), status));
  return report_outcome_internal(primary_id, secondary_id, outcome, status);
}

int live_model_impl::report_outcome(const char* primary_id, const char* secondary_id, float outcome, api_status* status)
{
  // Check arguments
  RETURN_IF_FAIL(check_null_or_empty(primary_id, _trace_logger.get(), status));
  RETURN_IF_FAIL(check_null_or_empty(secondary_id, _trace_logger.get(), status));
  return report_outcome_internal(primary_id, secondary_id, outcome, status);
}

int live_model_impl::refresh_model(api_status* status)
{
  if (_bg_model_proc)
  {
    RETURN_ERROR_LS(_trace_logger.get(), status, model_update_error)
        << "Cannot manually refresh model when backround polling is enabled";
  }

  model_management::model_data md;
  RETURN_IF_FAIL(_transport->get_data(md, status));

  bool model_ready = false;
  RETURN_IF_FAIL(_model->update(md, model_ready, status));

  _model_ready = model_ready;

  return error_code::success;
}

live_model_impl::live_model_impl(const utility::configuration& config, const error_fn fn, void* err_context,
    trace_logger_factory_t* trace_factory, data_transport_factory_t* t_factory, model_factory_t* m_factory,
    sender_factory_t* sender_factory, time_provider_factory_t* time_provider_factory)
    : _configuration(config)
    , _error_cb(fn, err_context)
    , _data_cb(_handle_model_update, this)
    , _watchdog(&_error_cb)
    , _trace_factory(trace_factory)
    , _t_factory{t_factory}
    , _m_factory{m_factory}
    , _sender_factory{sender_factory}
    , _time_provider_factory{time_provider_factory}
    , _protocol_version(_configuration.get_int(name::PROTOCOL_VERSION, value::DEFAULT_PROTOCOL_VERSION))
{
  // If there is no user supplied error callback, supply a default one that does nothing but report unhandled background
  // errors.
  if (fn == nullptr) { _error_cb.set(&default_error_callback, &_watchdog); }

  if (_configuration.get_bool(name::MODEL_BACKGROUND_REFRESH, value::DEFAULT_MODEL_BACKGROUND_REFRESH))
  {
    _bg_model_proc.reset(new utility::periodic_background_proc<model_management::model_downloader>(
        config.get_int(name::MODEL_REFRESH_INTERVAL_MS, 60 * 1000), _watchdog, "Model downloader", &_error_cb));
  }

  _learning_mode = learning::to_learning_mode(_configuration.get(name::LEARNING_MODE, value::LEARNING_MODE_ONLINE));
}

live_model_impl::live_model_impl(const utility::configuration& config, std::function<void(const api_status&)> error_cb,
    trace_logger_factory_t* trace_factory, data_transport_factory_t* t_factory, model_factory_t* m_factory,
    sender_factory_t* sender_factory, time_provider_factory_t* time_provider_factory)
    : _configuration(config)
    , _error_cb(std::move(error_cb))
    , _data_cb(_handle_model_update, this)
    , _watchdog(&_error_cb)
    , _trace_factory(trace_factory)
    , _t_factory{t_factory}
    , _m_factory{m_factory}
    , _sender_factory{sender_factory}
    , _time_provider_factory{time_provider_factory}
    , _protocol_version(_configuration.get_int(name::PROTOCOL_VERSION, value::DEFAULT_PROTOCOL_VERSION))
{
  if (_configuration.get_bool(name::MODEL_BACKGROUND_REFRESH, value::DEFAULT_MODEL_BACKGROUND_REFRESH))
  {
    _bg_model_proc.reset(new utility::periodic_background_proc<model_management::model_downloader>(
        config.get_int(name::MODEL_REFRESH_INTERVAL_MS, 60 * 1000), _watchdog, "Model downloader", &_error_cb));
  }

  _learning_mode = learning::to_learning_mode(_configuration.get(name::LEARNING_MODE, value::LEARNING_MODE_ONLINE));
}

int live_model_impl::init_trace(api_status* status)
{
  const auto* const trace_impl = _configuration.get(name::TRACE_LOG_IMPLEMENTATION, value::NULL_TRACE_LOGGER);
  i_trace* plogger = nullptr;
  RETURN_IF_FAIL(_trace_factory->create(&plogger, trace_impl, _configuration, nullptr, status));
  _trace_logger.reset(plogger);
  TRACE_INFO(_trace_logger, "API Tracing initialized");
  _watchdog.set_trace_log(_trace_logger.get());
  return error_code::success;
}

int live_model_impl::init_model(api_status* status)
{
  const auto* const model_impl = _configuration.get(name::MODEL_IMPLEMENTATION, value::VW);
  m::i_model* pmodel = nullptr;
  RETURN_IF_FAIL(_m_factory->create(&pmodel, model_impl, _configuration, _trace_logger.get(), status));
  _model.reset(pmodel);
  return error_code::success;
}

int live_model_impl::init_loggers(api_status* status)
{
  // Get the name of raw data (as opposed to message) sender for interactions.
  const auto* const ranking_sender_impl =
      _configuration.get(name::INTERACTION_SENDER_IMPLEMENTATION, value::get_default_interaction_sender());
  i_sender* ranking_data_sender = nullptr;

  // Use the name to create an instance of raw data sender for interactions
  _configuration.set(config_constants::CONFIG_SECTION, config_constants::INTERACTION);
  RETURN_IF_FAIL(_sender_factory->create(
      &ranking_data_sender, ranking_sender_impl, _configuration, &_error_cb, _trace_logger.get(), status));
  RETURN_IF_FAIL(ranking_data_sender->init(_configuration, status));

  // Create a message sender that will prepend the message with a preamble and send the raw data using the
  // factory created raw data sender
  l::i_message_sender* ranking_msg_sender = new l::preamble_message_sender(ranking_data_sender);
  RETURN_IF_FAIL(ranking_msg_sender->init(status));

  // Get time provider factory and implementation
  const auto* const time_provider_impl =
      _configuration.get(name::TIME_PROVIDER_IMPLEMENTATION, value::get_default_time_provider());

  i_time_provider* logger_extensions_time_provider = nullptr;
  RETURN_IF_FAIL(_time_provider_factory->create(
      &logger_extensions_time_provider, time_provider_impl, _configuration, _trace_logger.get(), status));

  // Create the logger extension
  _logger_extensions.reset(
      logger::i_logger_extensions::get_extensions(_configuration, logger_extensions_time_provider));

  i_time_provider* ranking_time_provider = nullptr;
  RETURN_IF_FAIL(_time_provider_factory->create(
      &ranking_time_provider, time_provider_impl, _configuration, _trace_logger.get(), status));

  // Create a logger for interactions that will use msg sender to send interaction messages
  _interaction_logger.reset(new logger::interaction_logger_facade(_model->model_type(), _configuration,
      ranking_msg_sender, _watchdog, ranking_time_provider, _logger_extensions.get(), &_error_cb));
  RETURN_IF_FAIL(_interaction_logger->init(status));

  // Get the name of raw data (as opposed to message) sender for observations.
  const auto* const outcome_sender_impl =
      _configuration.get(name::OBSERVATION_SENDER_IMPLEMENTATION, value::get_default_observation_sender());
  i_sender* outcome_sender = nullptr;

  // Use the name to create an instance of raw data sender for observations
  _configuration.set(config_constants::CONFIG_SECTION, config_constants::OBSERVATION);
  RETURN_IF_FAIL(_sender_factory->create(
      &outcome_sender, outcome_sender_impl, _configuration, &_error_cb, _trace_logger.get(), status));
  RETURN_IF_FAIL(outcome_sender->init(_configuration, status));

  // Create a message sender that will prepend the message with a preamble and send the raw data using the
  // factory created raw data sender
  l::i_message_sender* outcome_msg_sender = new l::preamble_message_sender(outcome_sender);
  RETURN_IF_FAIL(outcome_msg_sender->init(status));

  // Get time provider implementation
  i_time_provider* observation_time_provider = nullptr;
  RETURN_IF_FAIL(_time_provider_factory->create(
      &observation_time_provider, time_provider_impl, _configuration, _trace_logger.get(), status));

  // Create a logger for observations that will use msg sender to send observation messages
  _outcome_logger.reset(new logger::observation_logger_facade(
      _configuration, outcome_msg_sender, _watchdog, observation_time_provider, &_error_cb));
  RETURN_IF_FAIL(_outcome_logger->init(status));

  if (_configuration.get(name::EPISODE_EH_HOST, nullptr) != nullptr ||
      _configuration.get(name::EPISODE_FILE_NAME, nullptr) != nullptr ||
      _configuration.get(name::EPISODE_HTTP_API_HOST, nullptr) != nullptr)
  {
    // Get the name of raw data (as opposed to message) sender for episodes.
    const auto* const episode_sender_impl =
        _configuration.get(name::EPISODE_SENDER_IMPLEMENTATION, value::get_default_episode_sender());
    i_sender* episode_sender = nullptr;

    // Use the name to create an instance of raw data sender for episodes
    _configuration.set(config_constants::CONFIG_SECTION, config_constants::EPISODE);
    RETURN_IF_FAIL(_sender_factory->create(
        &episode_sender, episode_sender_impl, _configuration, &_error_cb, _trace_logger.get(), status));
    RETURN_IF_FAIL(episode_sender->init(_configuration, status));

    // Create a message sender that will prepend the message with a preamble and send the raw data using the
    // factory created raw data sender
    l::i_message_sender* episode_msg_sender = new l::preamble_message_sender(episode_sender);
    RETURN_IF_FAIL(episode_msg_sender->init(status));

    // Get time provider implementation
    i_time_provider* episode_time_provider = nullptr;
    RETURN_IF_FAIL(_time_provider_factory->create(
        &episode_time_provider, time_provider_impl, _configuration, _trace_logger.get(), status));

    // Create a logger for episodes that will use msg sender to send episode messages
    _episode_logger.reset(new logger::episode_logger_facade(
        _configuration, episode_msg_sender, _watchdog, episode_time_provider, &_error_cb));
    RETURN_IF_FAIL(_episode_logger->init(status));
  }

  return error_code::success;
}

void inline live_model_impl::_handle_model_update(const m::model_data& data, live_model_impl* ctxt)
{
  ctxt->handle_model_update(data);
}

void live_model_impl::handle_model_update(const model_management::model_data& data)
{
  if (data.refresh_count() == 0)
  {
    TRACE_INFO(_trace_logger, "Model was not updated since previous download");
    return;
  }

  api_status status;

  bool model_ready = false;

  if (_model->update(data, model_ready, &status) != error_code::success)
  {
    _error_cb.report_error(status);
    return;
  }
  _model_ready = model_ready;
}

int live_model_impl::init_model_mgmt(api_status* status)
{
  // Initialize transport for the model using transport factory
  const auto* const tranport_impl = _configuration.get(name::MODEL_SRC, value::get_default_data_transport());
  m::i_data_transport* ptransport = nullptr;
  RETURN_IF_FAIL(_t_factory->create(&ptransport, tranport_impl, _configuration, status));
  // This class manages lifetime of transport
  this->_transport.reset(ptransport);

  if (_bg_model_proc)
  {
    // Initialize background process and start downloading models
    this->_model_download.reset(new m::model_downloader(ptransport, &_data_cb, _trace_logger.get()));
    return _bg_model_proc->init(_model_download.get(), status);
  }

  return refresh_model(status);
}

int live_model_impl::request_episodic_decision(const char* event_id, const char* previous_id, string_view context_json,
    unsigned int flags, ranking_response& resp, episode_state& episode, api_status* status)
{
  resp.clear();
  // clear previous errors if any
  api_status::try_clear(status);

  // check arguments
  RETURN_IF_FAIL(check_null_or_empty(event_id, context_json, _trace_logger.get(), status));
  const uint64_t seed = VW::uniform_hash(event_id, strlen(event_id), 0) + _seed_shift;

  std::vector<int> action_ids;
  std::vector<float> action_pdf;
  std::string model_version;

  const auto history = episode.get_history();
  const std::string context_patched = history.get_context(previous_id, context_json);

  RETURN_IF_FAIL(_model->choose_rank_multistep(
      event_id, seed, context_patched.c_str(), history, action_ids, action_pdf, model_version, status));
  RETURN_IF_FAIL(sample_and_populate_response(
      seed, action_ids, action_pdf, std::move(model_version), resp, _trace_logger.get(), status));

  resp.set_event_id(event_id);

  RETURN_IF_FAIL(episode.update(event_id, previous_id, context_json, resp, status));

  if (episode.size() == 1)
  {
    // Log the episode id when starting a new episode
    RETURN_IF_FAIL(_episode_logger->log(episode.get_episode_id(), status));
  }
  RETURN_IF_FAIL(
      _interaction_logger->log(episode.get_episode_id(), previous_id, context_patched.c_str(), flags, resp, status));

  return error_code::success;
}

// helper: check if at least one of the arguments is null or empty
int check_null_or_empty(const char* arg1, string_view arg2, i_trace* trace, api_status* status)
{
  if ((arg1 == nullptr) || strlen(arg1) == 0 || arg2.empty())
  {
    RETURN_ERROR_ARG(trace, status, invalid_argument, "one of the arguments passed to the ds is null or empty");
  }
  return error_code::success;
}

int check_null_or_empty(string_view arg1, i_trace* trace, api_status* status)
{
  if (arg1.empty())
  {
    RETURN_ERROR_ARG(trace, status, invalid_argument, "one of the arguments passed to the ds is null or empty");
  }
  return error_code::success;
}

int check_null_or_empty(const char* arg1, i_trace* trace, api_status* status)
{
  if ((arg1 == nullptr) || strlen(arg1) == 0)
  {
    RETURN_ERROR_ARG(trace, status, invalid_argument, "one of the arguments passed to the ds is null or empty");
  }
  return error_code::success;
}

int reset_action_order(ranking_response& response)
{
  std::sort(response.begin(), response.end(),
      [](const action_prob& a, const action_prob& b) { return a.action_id < b.action_id; });
  RETURN_IF_FAIL(response.set_chosen_action_id((*(response.begin())).action_id));

  return error_code::success;
}

int reset_chosen_action_multi_slot(multi_slot_response& response, const std::vector<int>& baseline_actions)
{
  uint32_t index = 0;
  for (auto& slot : response)
  {
    if (!baseline_actions.empty() && baseline_actions.size() >= index) { slot.set_action_id(baseline_actions[index]); }
    else
    {
      // implicit baseline is the action corresponding to the slot index
      slot.set_action_id(index);
    }
    slot.set_probability(1.f);
    ++index;
  }
  return error_code::success;
}

int reset_chosen_action_multi_slot(multi_slot_response_detailed& response, const std::vector<int>& baseline_actions)
{
  size_t index = 0;
  for (auto& slot : response)
  {
    if (!baseline_actions.empty() && baseline_actions.size() >= index)
    {
      RETURN_IF_FAIL(slot.set_chosen_action_id(baseline_actions[index]));
    }
    else
    {
      // implicit baseline is the action corresponding to the slot index
      RETURN_IF_FAIL(slot.set_chosen_action_id(index));
    }
    ++index;
  }
  return error_code::success;
}

void autogenerate_missing_uuids(
    const std::map<size_t, std::string>& found_ids, std::vector<std::string>& complete_ids, uint64_t seed_shift)
{
  for (const auto& ids : found_ids) { complete_ids[ids.first] = ids.second; }

  for (auto& complete_id : complete_ids)
  {
    if (complete_id.empty())
    {
      complete_id = boost::uuids::to_string(boost::uuids::random_generator()()) + std::to_string(seed_shift);
    }
  }
}
}  // namespace reinforcement_learning
