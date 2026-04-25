#include "include/model.h"

namespace jiuwen {

Model::Model(ModelConfig config) : config_(std::move(config)){} 

ModelConfig Model::GetConfig() const 
{ 
    return config_; 
}

} // namespace jiuwen
