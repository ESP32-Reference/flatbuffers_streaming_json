#pragma once

#include "picojson.h"

#include <vector>
#include <sstream>

#include "flatbuffers/idl.h"
// read streaming JSON into dynamically built flatbuffer:
#include "flatbuffers/reflection.h"
// simple output to JSON string:
#include "flatbuffers/minireflect.h"

#include "stx/string_view.hpp"

#include "esp_log.h"

inline bool
equality_or_wildcard(
  stx::string_view root,
  stx::string_view current
);

inline bool
is_a_subpath(
  const std::vector<std::string>& current_path,
  const std::vector<std::string>& root_path
);

constexpr char wildcard_sym[] = "*";

template<typename MessageT, typename ErrorT>
class FlatbuffersStreamingParser
{
private:
  FlatbuffersStreamingParser(const FlatbuffersStreamingParser &);
  FlatbuffersStreamingParser &operator=(const FlatbuffersStreamingParser &);

  const char TAG[27] = "FlatbuffersStreamingParser";

  std::vector<std::string> root_path;
  std::function<bool(const MessageT&)> callback;

  std::vector<std::string> error_path;
  std::function<bool(const ErrorT&)> errback;

  flatbuffers::Parser parser;
  bool flatbuffers_parser_ready = false;

  bool is_parse_error = false;

  // Trigger errback instead of callback when error path matches
  bool is_error_path = false;

  // Input parsing state
  int object_depth = 0;
  int array_depth = 0;
  int object_idx = 0;
  int array_idx = 0;
  std::vector<std::string> current_path;
  std::string current_key;

  // Output state
  std::ostringstream ss;
  bool emit_json = false;
  bool needs_close_array = false;
  bool needs_close_object = false;

  // Reflection state
  const reflection::Schema* schema;
  const reflection::Object* reflection_table;

public:
  FlatbuffersStreamingParser(
    stx::string_view text_schema,
    stx::string_view binary_schema
  )
  {
    clear();

    // Allow trailing commas, and optional quotes around identifiers/values
    parser.opts.strict_json = false;

    // Support additional (ignored) fields present in JSON but not in the schema
    parser.opts.skip_unexpected_fields_in_json = true;

    if (parse_flatbuffers_text_schema(text_schema))
    {
      // Success
      ESP_LOGI(TAG, "Successfully parsed text flatbuffer schema buffer");
      flatbuffers_parser_ready = true;
    }

    if (parse_flatbuffers_binary_schema(binary_schema))
    {
      // Success
      ESP_LOGI(TAG, "Successfully parsed binary flatbuffer schema buffer");

      // Print the namespaced-name of the (default) root object
      reflection_table = schema->root_table();
      ESP_LOGI(TAG, "Default root table: %s", schema->root_table()->name()->c_str());
    }
  }

  void clear()
  {
    // Reset error state
    is_parse_error = false;

    // Trigger errback instead of callback when error path matches
    is_error_path = false;

    // Input parsing state
    object_depth = 0;
    array_depth = 0;
    object_idx = 0;
    array_idx = 0;
    current_path.clear();
    current_key.clear();

    // Output state
    ss.str("");
    emit_json = false;
    needs_close_array = false;
    needs_close_object = false;
  }

  bool parse_stream(
    const std::istream& resp,
    const std::vector<std::string>& _root_path={},
    std::function<bool(const MessageT&)> _callback=nullptr,
    const std::vector<std::string>& _error_path={},
    std::function<bool(const ErrorT&)> _errback=nullptr
  )
  {
    std::string err;

    // Reset existing state
    clear();

    root_path = _root_path;
    callback = _callback;
    error_path = _error_path;
    errback = _errback;

    picojson::_parse(
      *this,
      std::istreambuf_iterator<char>(resp.rdbuf()),
      std::istreambuf_iterator<char>(),
      &err);

    if (!err.empty())
    {
      ESP_LOGE(TAG, "Unable to parse JSON response, err = %s", err.c_str());
    }

    return (
      (err.empty() == true) &&
      (is_parse_error == false)
    );
  }

  bool
  set_null()
  {
    if (emit_json)
    {
      ss << "null";
    }
    return true;
  }

  bool
  set_bool(bool b)
  {
    if (emit_json)
    {
      ss << (b? "true" : "false");
    }
    return true;
  }

  bool
  set_int64(int64_t i)
  {
    if (emit_json)
    {
      ss << i;
    }
    return true;
  }

  bool
  set_number(double d)
  {
    if (emit_json)
    {
      //ss << d;
      ss << (int)(d);
    }
    return true;
  }

  template <typename Iter> bool
  parse_string(picojson::input<Iter> &in)
  {
    std::string s;
    auto ok = _parse_string(s, in);
    if (emit_json)
    {
      ss << "\"" << s << "\"";
    }
    return ok;
  }

  bool
  parse_array_start()
  {
    array_depth++;
    array_idx = 0;

    if (emit_json)
    {
      ss << "[";
    }
    return true;
  }

  template <typename Iter> bool 
  parse_array_item(picojson::input<Iter> &in, size_t i)
  {
    // print leading comma (it should have followed last parsed item)
    if (emit_json)
    {
      if (array_idx > 0)
      {
        ss << ",";
      }
    }

    array_idx++;
    return _parse(*this, in);
  }

  bool
  parse_array_stop(size_t)
  {
    array_depth--;
    array_idx = -1;

    if (emit_json)
    {
      ss << "]";
    }
    return true;
  }

  bool
  parse_object_start()
  {
    object_depth++;
    object_idx = 0;

    // We can lookahead to the fields first in parse_object_item
    // If needed, an object '{' will be opened there
    return true;
  }

  template <typename Iter> bool
  parse_object_item(
    picojson::input<Iter> &in,
    const std::string &key)
  {
    // Store the previous reflection table,
    // In case we recurse into a reflection sub-table
    auto reflection_table_prev = reflection_table;

    // Check whether we used a workaround to reformat the JSON
    // To be more flatbuffers friendly
    bool keyed_vector_table_found = check_for_keyed_vector_table(key);

    // Capture and push the current object key onto the current path
    current_key = key;
    current_path.push_back(current_key);

    // Check for error path first
    is_error_path = is_a_subpath(current_path, error_path);

    auto emit_json_prev = emit_json;
    // Start outputting JSON if either with error or message path
    emit_json = (is_error_path || is_a_subpath(current_path, root_path));

    if (emit_json)
    {
      if (keyed_vector_table_found)
      {
        if (!emit_json_prev)
        {
          // Start (or ignore starting) an anonymous array here.
        }
        else if (!needs_close_array)
        {
          ss << "[";
          needs_close_array = true;
        }
        else {
          ss << ",";
        }

        // Print key with array/object indirection
        ss << "{\"id\":\"" << key << "\",\"val\":";
        needs_close_object = true;
      }
      else {
        if (!needs_close_object)
        {
          ss << "{";
          needs_close_object = true;
        }
        else {
          ss << ",";
        }

        // Print key
        ss << "\"" << key << "\":";
      }
    }

    auto needs_close_array_prev = needs_close_array;
    auto needs_close_object_prev = needs_close_object;
    needs_close_array = false;
    needs_close_object = false;

    auto ok = _parse(*this, in);

    needs_close_array = needs_close_array_prev;
    needs_close_object = needs_close_object_prev;
    reflection_table = reflection_table_prev;

    object_idx++;

    // pop the key, it has now been parsed
    current_path.pop_back();

    if (is_a_subpath(current_path, root_path))
    {
    }
    else {
      if (emit_json) // check if we were emitting
      {
        if (keyed_vector_table_found == false)
        {
          // We will be missing one of these at this point in regular parsing
          ss << "}";
        }

        if (process_item() == false)
        {
          is_parse_error = true;
        }
      }
      emit_json = false;
    }

    return ok;
  }

  bool
  parse_object_stop()
  {
    current_key.clear();

    object_depth--;
    object_idx = -1;

    if (emit_json)
    {
      if (needs_close_array)
      {
        ss << "]";
        needs_close_array = false;
      }
      else if (needs_close_object)
      {
        // We did expect at least one of these
        ss << "}";

        if (!root_path.empty())
        {
          // We will be missing one of these at this point in regular parsing
          ss << "}";
        }
        needs_close_object = false;
      }
      else {
        ss << "}";
      }

      if (object_depth == 0)
      {
        process_item();
      }
    }

    return true;
  }

  bool
  process_item()
  {
    bool ok = convert_json_stream_to_flatbuffer();

    // reset the JSON output stream
    ss.str("");

    return ok;
  }

  bool
  check_for_keyed_vector_table(const std::string& key)
  {
    if (reflection_table != nullptr)
    {
      auto fields = reflection_table->fields();
      if (fields != nullptr)
      {
        auto id_field = fields->LookupByKey("id");
        auto val_field = fields->LookupByKey("val");

        if ((id_field != nullptr) &&
            (val_field != nullptr))
        {
          if (val_field->type()->base_type() == reflection::Obj)
          {
            reflection_table = schema->objects()->Get(
              val_field->type()->index()
            );

            if (reflection_table != nullptr)
            {
              // We found a reflection structure that can be re-written
              return true;
            }
          }
        }
        else {
          auto field = fields->LookupByKey(key.c_str());
          if (field != nullptr)
          {
            if (field->type()->base_type() == reflection::Obj)
            {
              //TODO(@paulreimer): could be a union/struct? what then?
              reflection_table = schema->objects()->Get(
                field->type()->index()
              );
            }

            if (field->type()->base_type() == reflection::Vector)
            {
              if (field->type()->element() == reflection::Obj)
              {
                reflection_table = schema->objects()->Get(
                  field->type()->index()
                );

                if (reflection_table != nullptr)
                {
                  //TODO(@paulreimer): good state, but should this be logged somehow?
                }
              }
            }
          }
        }
      }
    }

    return false;
  }

  bool
  parse_flatbuffers_text_schema(stx::string_view buf)
  {
    // Load flatbuffers text schema file from buffer
    // Check for a non-zero buffer length
    if (!buf.empty())
    {
      // Check for a nullptr terminated buffer
      char eof = buf[buf.size() - 1];
      if (eof == 0x00)
      {
        bool ok = parser.Parse(buf.data(), nullptr);
        if (ok)
        {
          return true;
        }
        else {
          ESP_LOGE(TAG, "Invalid text flatbuffer schema buffer");
        }
      }
      else {
        ESP_LOGE(TAG, "nullptr byte missing from text flatbuffer schema buffer");
      }
    }
    else {
      ESP_LOGE(TAG, "0 length text flatbuffer schema buffer found");
    }

    return false;
  }

  bool
  parse_flatbuffers_binary_schema(stx::string_view buf)
  {
    // Load flatbuffers text schema file from buffer
    // Check for a non-zero buffer length
    if (!buf.empty())
    {
      // Check for a nullptr terminated buffer
      char eof = buf[buf.size() - 1];
      if (eof == 0x00)
      {
        // Verify the buffer as valid flatbuffer
        flatbuffers::Verifier verifier(
          reinterpret_cast<const uint8_t *>(buf.data()), buf.size());
        if (reflection::VerifySchemaBuffer(verifier))
        {
          // Parse a buffer containing the binary schema
          schema = reflection::GetSchema(buf.data());
          if (schema != nullptr)
          {
            return true;
          }
        }
        else {
          ESP_LOGE(TAG, "Invalid binary flatbuffer schema buffer");
        }
      }
      else {
        ESP_LOGE(TAG, "nullptr byte missing from text flatbuffer schema buffer");
      }
    }
    else {
      ESP_LOGE(TAG, "0 length binary flatbuffer schema buffer found");
    }

    return false;
  }

  bool
  convert_json_stream_to_flatbuffer()
  {
    bool ok = false;

    // Attempt to parse the JSON stream into a flatbuffer of template type
    if (flatbuffers_parser_ready)
    {
      // Determine whether to expect to parse an Error type or a Message type
      ok = parser.SetRootType(
        (is_error_path)?
        ErrorT::TableType::GetFullyQualifiedName() :
        MessageT::TableType::GetFullyQualifiedName()
      );

      // We are set up for the root type of flatbuffer now
      if (ok)
      {
        // Parse JSON output stream into flatbuffer
        ok = parser.Parse(ss.str().c_str(), nullptr);
        if (ok)
        {
          // Here, parser.builder_ contains a binary buffer
          // that is the finished parsed data.
          // Create a generatic verifier for it
          flatbuffers::Verifier verifier(
            parser.builder_.GetBufferPointer(),
            parser.builder_.GetSize());

          // Take the callback path
          if (!is_error_path)
          {
            // Verify as valid message type
            ok = verifier.VerifyBuffer<
              typename MessageT::TableType>(
                nullptr);
            if (ok)
            {
              // Instantiate a C++ gen-object-api object for the message
              MessageT message;
              const auto flatbuf = flatbuffers::GetRoot<
                typename MessageT::TableType>(
                  parser.builder_.GetBufferPointer()
              );

              // Unpack the binary into the C++ object
              flatbuf->UnPackTo(&message);

              // Trigger the callback
              ok = callback(message);
            }
            else {
              ESP_LOGE(TAG, "Couldn't verify flatbuffer message type");
            }
          }
          // Take the errback path
          else {
            // Verify as valid error type
            ok = verifier.VerifyBuffer<typename ErrorT::TableType>(nullptr);
            if (ok)
            {
              // Instantiate a C++ gen-object-api object for the error
              ErrorT error;
              const auto flatbuf = flatbuffers::GetRoot<
                typename ErrorT::TableType>(
                  parser.builder_.GetBufferPointer()
              );

              // Unpack the binary into the C++ object
              flatbuf->UnPackTo(&error);

              // Trigger the errback
              ok = errback(error);
            }
            else {
              ESP_LOGE(TAG, "Couldn't verify flatbuffer error type");
            }
          }
        }
        else {
          ESP_LOGE(TAG,
            "Couldn't parse JSON string '%s' into valid flatbuffer of type '%s'",
            ss.str().c_str(), MessageT::GetFullyQualifiedName()
          );
        }
      }
      else {
        ESP_LOGE(TAG, "Could not set flatbuffer root type '%s'", MessageT::GetFullyQualifiedName());
      }
    }

    return ok;
  }
};

inline bool
equality_or_wildcard(
  stx::string_view root,
  stx::string_view current)
{
  return ((root == wildcard_sym) || (root == current));
}

inline bool
is_a_subpath(
  const std::vector<std::string>& current_path,
  const std::vector<std::string>& root_path
)
{
  return (
    (root_path.empty()) || (
    (current_path.size() >= root_path.size()) &&
    (std::equal(
      root_path.begin(),
      root_path.end(),
      current_path.begin(),
      equality_or_wildcard
    ))
  ));
}