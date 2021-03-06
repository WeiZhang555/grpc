# Generated by the protocol buffer compiler.  DO NOT EDIT!
# Source: route_guide.proto for package 'routeguide'

require 'grpc'
require 'route_guide'

module Routeguide
  module RouteGuide

    # TODO: add proto service documentation here
    class Service

      include GRPC::GenericService

      self.marshal_class_method = :encode
      self.unmarshal_class_method = :decode
      self.service_name = 'routeguide.RouteGuide'

      rpc :GetFeature, Point, Feature
      rpc :ListFeatures, Rectangle, stream(Feature)
      rpc :RecordRoute, stream(Point), RouteSummary
      rpc :RouteChat, stream(RouteNote), stream(RouteNote)
    end

    Stub = Service.rpc_stub_class
  end
end
