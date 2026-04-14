#include "../include/model.h"

Model::Model(ModelConfig config) : config_(std::move(config)) {}
ModelConfig Model::GetConfig() const { return config_; }
