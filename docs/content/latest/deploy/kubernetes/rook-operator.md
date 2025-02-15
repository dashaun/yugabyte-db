---
title: Rook YugabyteDB operator
linkTitle: Rook operator
description: Rook YugabyteDB operator
menu:
  latest:
    identifier: rook-operator
    parent: deploy-kubernetes
    weight: 624
isTocNested: true
showAsideToc: true
---

[Rook](https://rook.io) is an open source, cloud-native storage orchestrator for Kubernetes, providing the platform, framework, and support that can turn YugabyteDB clusters into self-managing, self-scaling, and self-healing storage services. Rook automates storage-layer tasks, including deployment, bootstrapping, configuration, provisioning, scaling, upgrading, migration, disaster recovery, monitoring, and resource management.

The [Rook YugabyteDB operator](https://rook.io/docs/rook/v1.1/yugabytedb.html) is a custom controller that uses Custom Resource Definition (CRD) to extend the Kubernetes API and automate deploying, scaling, and managing YugabyteDB clusters.  Based on the  _desired state_ that you specified in the CRD, the Rook operator observes (watching for changes in state and health), analyzes (comparing current to desired state), and acts (applying changes to the cluster) to maintain the desired state. For details, see [YugabyteDB Cluster CRD](https://rook.io/docs/rook/v1.1/yugabytedb-cluster-crd.html)

## Before you begin

A YugabyteDB cluster installed in a Kubernetes environment is required.

- To create a local cluster in Kubernetes for development and learning, see [Quick start](https://docs.yugabyte.com/latest/quick-start/).
- To deploy a production cluster, see the YugabyteDB documentation on [deploying in  Kubernetes](../kubernetes/).

Verify that your Kubernetes cluster is ready for Rook by reviewing the [Kubernetes cluster prerequisites for using the Rook operator](https://github.com/rook/rook/blob/master/Documentation/k8s-pre-reqs.md).

Rook must be installed — see the [Rook GitHub Repository](https://github.com/rook/rook). You can install Rook by running the following command:

```bash
git clone git@github.com:rook/rook.git
```

## Deploy the Rook YugabyteDB operator

To deploy the YugabyteDB operator:

1. Change your directory to the Rook directory containing the YugabyteDB example files.

    ```bash
    cd cluster/examples/kubernetes/yugabytedb
    ```

2. Create the Rook YugabyteDB operator by running the following command:

    ```bash
    kubectl create -f operator.yaml
    ```

3. Observe the Rook operator by running the following command:

    ```bash
    kubectl -n rook-yugabytedb-system get pods
    ```

## Create the YugabyteDB Cluster CRD

When using the Rook YugabyteDB operator, your YugabyteDB clusters are controlled using the custom resource object (`ybclusters.yugabytedb.rook.io`). The Custom Resource Definition (CRD), used to create this object, is specified in the `cluster.yaml` file.  

A sample Custom Resource Definition (CRD) file, called `cluster.yaml`, is located in the following Rook directory:

```bash
cluster/examples/kubernetes/yugabytedb
```

Make a copy of the sample CRD file (`cluster.yaml`)  and modify it as needed. For details on the configuration options, see [YugabyteDB CRD](https://rook.io/docs/rook/v1.1/yugabytedb-cluster-crd.html).

## Create a simple YugabyteDB cluster

1. Create your YugabyteDB cluster by running the following command:

    ```bash
    kubectl create -f cluster.yaml
    ```

2. Verify that the custom resource object was created by using the following command:

    ```bash
    kubectl -n rook-yugabytedb get ybclusters.yugabytedb.rook.io
    ```

3. Verify that the number of YB-Master and YB- TServer services that are running match the number you specified in the `cluster.yaml` file by running the following command:

    ```bash
    kubectl -n rook-yugabytedb get pods
    ```

## Use YugabyteDB

When all of the pods in YugabyteDB cluster are running, you can use the YSQL shell to access the YSQL API, which is PostgreSQL-compliant.

```console
kubectl exec -it yb-tserver-rook-yugabytedb-0 /home/yugabyte/bin/ysqlsh -- -h yb-tserver-rook-yugabytedb-0  --echo-queries
```

For details on the YSQL API, see:

- [Explore YSQL](../../../quick-start/explore-ysql/#kubernetes)
- [YSQL Reference](../../../api/ysql/) 

## Cleanup

Run the commands below to clean up all resources created above.

**NOTE:** This will destroy your database and delete all of its data.

```console
kubectl delete -f cluster.yaml
kubectl delete -f operator.yaml
```

Manually delete any Persistent Volumes that were created for this YugabyteDB cluster.

## Troubleshooting

### Review the operator logs

If the cluster does not start,  run following command to take a look at operator logs.

```bash
kubectl -n rook-yugabytedb-system logs -l app=rook-yugabytedb-operator
```

### Review the YugabyteDB logs

If everything is OK in the operator logs, check the YugabyteDB logs for YB-Master and YB-TServer.

```bash
kubectl -n rook-yugabytedb logs -l app=yb-master-rook-yugabytedb
kubectl -n rook-yugabytedb logs -l app=yb-tserver-rook-yugabytedb
```
