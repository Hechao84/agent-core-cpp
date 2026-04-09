#include "../include/model.h"

Model::Model(ModelConfig config) : m_config(std::move(config)) {}
ModelConfig Model::GetConfig() const { return m_config; }
