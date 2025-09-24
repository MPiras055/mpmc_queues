#! usr/bin/env/python3
import os
import sys
from typing import List, Optional, Tuple, Any
from dataclasses import dataclass
from pathlib import Path
import json

CPU_PATH = '/sys/devices/system/cpu'
NODE_PATH = '/sys/devices/system/node'


@dataclass
class CPUInfo:
    """Data class to represent CPU information"""
    logical_id: int
    core_id: int
    siblings: List[int]
    numa_node: Optional[int]
    cache_ids: List[int]
    package_id: int


@dataclass
class NodeInfo:
    """Data class to represent NUMA node information"""
    id: int
    memory: bool
    cores_list: list[tuple[int, ...]]
    distance: list[int]


class SystemTopology:
    """Class to encapsulate system topology operations"""

    def __init__(self):
            self.cpu_path: Path = Path(CPU_PATH)
            self.node_path: Path = Path(NODE_PATH)

    def get_cpu_info(self) -> list[CPUInfo]:
            """
            Returns a list of CPUInfo objects containing information about each CPU
            including physical core id, logical id, cache ids, etc.
            """
            try:
                cpu_entries = [f for f in self.cpu_path.iterdir()
                              if f.name.startswith("cpu") and f.name[3:].isdigit()]
            except (OSError, FileNotFoundError):
                print(f"Error: Unable to access {CPU_PATH}")
                return []

            cpu_list: list[CPUInfo] = []

            for cpu_entry in cpu_entries:
                cpu_nbr = int(cpu_entry.name[3:])
                topology_dir = cpu_entry / "topology"

                try:
                    # Read siblings list
                    siblings_file = topology_dir / "thread_siblings_list"
                    with siblings_file.open('r') as f:
                        siblings = [int(x) for x in f.read().strip().split(',') if x.strip()]

                    # Read the core_id
                    core_id_file = topology_dir / "core_id"
                    with core_id_file.open('r') as f:
                        core_id = int(f.read().strip())

                    # Read the package_id
                    package_id_file = topology_dir / "physical_package_id"
                    with package_id_file.open('r') as f:
                        package_id = int(f.read().strip())

                    # Gather cache and numa information
                    cache_ids = self._get_cache_info(cpu_nbr)
                    numa_node = self._get_node_of_cpu(cpu_nbr)

                    cpu_info = CPUInfo(
                        logical_id=cpu_nbr,
                        core_id=core_id,
                        siblings=siblings,
                        numa_node=numa_node,
                        cache_ids=cache_ids,
                        package_id=package_id
                    )
                    cpu_list.append(cpu_info)

                except (IOError, FileNotFoundError, ValueError) as e:
                    print(f"Warning: Could not read complete information for {cpu_entry.name}: {str(e)}")
                    continue

            cpu_list.sort(key=lambda x: x.logical_id)
            return cpu_list

    def _get_node_of_cpu(self, logical_id: int) -> int|None:
        """Returns the numa node of the given logical CPU ID"""
        try:
            cpu_dir = self.cpu_path / f"cpu{logical_id}"
            for file_path in cpu_dir.iterdir():
                if file_path.name.startswith("node") and file_path.name[4:].isdigit():
                    return int(file_path.name[4:])
        except (IOError, FileNotFoundError):
            pass
        return None

    def _get_cpus_logical_ids(self, logical_id: int) -> list[int]:
        """Returns the list of logical ids of the CPUs (SMT siblings) of the given core"""
        try:
            path = self.cpu_path / f"cpu{logical_id}" / "topology" / "thread_siblings_list"
            with path.open('r') as f:
                return [int(i) for i in f.read().strip().split(',')]
        except (IOError, FileNotFoundError):
            return []

    def _get_cache_info(self, logical_id: int) -> list[int]:
        """Returns the list of cache_ids for all cache levels of a given CPU"""
        cache_ids: list[int] = []

        try:
            cache_path = self.cpu_path / f"cpu{logical_id}" / "cache"

            # Get all cache index directories and sort them
            cache_dirs: list[Path] = [d for d in cache_path.iterdir()
                         if d.name.startswith("index") and d.name[5:].isdigit()]
            cache_dirs.sort(key=lambda x: int(x.name[5:]))

            # Read cache IDs for each level
            for cache_dir in cache_dirs:
                id_file = cache_dir / "id"
                with id_file.open('r') as f:
                    cache_ids.append(int(f.read().strip()))

        except (IOError, FileNotFoundError):
            pass

        return cache_ids

    def get_nodes_info(self) -> list[NodeInfo]:
        """
        Returns a list of NodeInfo objects containing:
        - node_id: int
        - memory: bool (whether the node has memory)
        - cores_list: list of CPUs (logical IDs)
        - distance: list of distances to other nodes
        """
        memory_nodes: set[int]

        try:
            # Get nodes with memory
            memory_file = self.node_path / "has_memory"
            with memory_file.open('r') as f:
                memory_nodes = {int(i) for i in f.read().strip().split(",")}
        except (IOError, FileNotFoundError):
            print("Warning: Could not read memory node information")
            memory_nodes = set()

        nodes: list[NodeInfo] = []

        try:
            node_dirs = [d for d in self.node_path.iterdir()
                        if d.name.startswith("node") and d.name[4:].isdigit()]
        except (OSError, FileNotFoundError):
            print(f"Error: Unable to access {NODE_PATH}")
            return []

        for node_dir in node_dirs:
            node_id = int(node_dir.name[4:])

            try:
                # Get CPU list for this node
                cpus_file = node_dir / "cpulist"
                with cpus_file.open('r') as f:
                    cpus_data = f.read().strip()

                # Parse CPU range or list
                if '-' in cpus_data:
                    start, end = map(int, cpus_data.split("-"))
                    cpus = list(range(start, end + 1))
                else:
                    cpus = [int(i) for i in cpus_data.split(",") if i.strip()]

                # Get distance matrix
                distance_file = node_dir / "distance"
                with distance_file.open('r') as f:
                    distances = [int(i) for i in f.read().strip().split()]

                # Get logical IDs for each core (including SMT siblings)
                cores_list: list[tuple[int,...]] = []
                for core in cpus:
                    siblings = self._get_cpus_logical_ids(core)
                    if siblings:
                        cores_list.append(tuple(siblings))

                # Remove duplicates while preserving order
                seen: set[tuple[int,...]] = set()
                unique_cores: list[tuple[int,...]] = []
                for core in cores_list:
                    if core not in seen:
                        seen.add(core)
                        unique_cores.append(core)

                node_info = NodeInfo(
                    id=node_id,
                    memory=node_id in memory_nodes,
                    cores_list=unique_cores,
                    distance=distances
                )
                nodes.append(node_info)

            except (IOError, FileNotFoundError, ValueError) as e:
                print(f"Warning: Could not read node {node_id} information: {str(e)}")
                continue

        nodes.sort(key=lambda x: x.id)
        return nodes

    def optimize_cache_nodes(self, node: NodeInfo, cache_level: int) -> NodeInfo:
        """
        Reorganizes cores in a node to group them by shared cache at the specified level.
        """
        if cache_level < 0:
            return node

        cores_with_cache = []

        for core_tuple in node.cores_list:
            if not core_tuple:
                continue

            cache_ids = self._get_cache_info(core_tuple[0])

            if not cache_ids or cache_level >= len(cache_ids):
                continue

            cores_with_cache.append({
                'core_tuple': core_tuple,
                'cache_id': cache_ids[cache_level],
                'first_logical_id': core_tuple[0]
            })

        # Sort by cache ID and then by first logical ID
        cores_with_cache.sort(key=lambda x: (x['cache_id'], x['first_logical_id']))

        # Update the node's cores list
        node.cores_list = [core['core_tuple'] for core in cores_with_cache]
        return node

    def get_clusters(self, nodes: list[NodeInfo]|None = None,
                    optimize_cache_level: int = -1) -> list[list[NodeInfo]]:
        """
        Identifies clusters of NUMA nodes based on memory proximity.
        """
        if nodes is None:
            nodes = self.get_nodes_info()

        if not nodes:
            return []

        # Get all memory node IDs
        memory_nodes = [node for node in nodes if node.memory]

        if not memory_nodes:
            print("Warning: No memory nodes found!")
            return [nodes]

        # Create clusters with memory nodes
        clusters = {str(mem_node.id): [mem_node] for mem_node in memory_nodes}

        # For each non-memory node, find closest memory node
        for node in nodes:
            if node.memory:
                continue

            min_dist = float('inf')
            closest_mem_id = None

            for mem_node in memory_nodes:
                if mem_node.id < len(node.distance):
                    dist = node.distance[mem_node.id]
                    if dist < min_dist:
                        min_dist = dist
                        closest_mem_id = str(mem_node.id)

            if closest_mem_id:
                clusters[closest_mem_id].append(node)

        # Sort nodes in each cluster by distance to memory node
        for cluster in clusters.values():
            if len(cluster) <= 1:
                continue

            memory_node = cluster[0]
            cluster[1:] = sorted(
                cluster[1:],
                key=lambda x: (x.distance[memory_node.id]
                              if memory_node.id < len(x.distance)
                              else float('inf'))
            )

        # Apply cache optimization if requested
        if optimize_cache_level >= 0:
            for cluster in clusters.values():
                for i in range(len(cluster)):
                    cluster[i] = self.optimize_cache_nodes(cluster[i], optimize_cache_level)

        return list(clusters.values())

    def save_cluster_info(self, filename: str, cache_level: int = 3) -> bool:
        """Saves CPU pinning information to a file."""
        try:
            clusters = self.get_clusters(optimize_cache_level=cache_level)

            if not clusters:
                print("Error: No clusters found")
                return False

            with open(filename, 'w') as f:
                f.write("# CPU Pinning Configuration\n")
                f.write(f"# Generated on {os.uname().nodename}\n")
                f.write("# Format: core[cluster_id][node_id][core_id]=smt_list\n")
                f.write(f"# Cache optimization level: {cache_level}\n\n")

                for cluster_idx, cluster in enumerate(clusters):
                    f.write(f"# Cluster {cluster_idx} (Memory Node: {cluster[0].id})\n")

                    for node_idx, node in enumerate(cluster):
                        f.write(f"# Node {node.id} ({'Memory' if node.memory else 'Compute'})\n")

                        for core_idx, cpus in enumerate(node.cores_list):
                            if cpus:
                                core_name = f"core[{cluster_idx}][{node_idx}][{core_idx}]"
                                cpu_list = ",".join(map(str, cpus))
                                f.write(f"{core_name}={cpu_list}\n")

            print(f"CPU pinning configuration saved to {filename}")
            return True

        except Exception as e:
            print(f"Error saving pinning file: {str(e)}")
            return False


class CPUPinningGenerator:
    """Class for generating different CPU pinning strategies"""

    def __init__(self, topology: SystemTopology):
        self.topology = topology

    def cluster_first_pinning(self, clusters: List[List[NodeInfo]]) -> List[int]:
        """
        Generates CPU pinning that fills clusters completely before moving to the next.
        For each SMT position, processes all cores in all nodes in all clusters.
        """
        if not clusters or not clusters[0] or not clusters[0][0].cores_list:
            return []

        cpu_list = []

        # Determine max SMT count
        max_smt = max(len(core) for cluster in clusters
                     for node in cluster
                     for core in node.cores_list)

        # For each SMT position
        for smt_pos in range(max_smt):
            # For each cluster
            for cluster in clusters:
                # For each node in the cluster
                for node in cluster:
                    # For each core in the node
                    for core in node.cores_list:
                        # If this core has this SMT position
                        if smt_pos < len(core):
                            cpu_list.append(core[smt_pos])

        return cpu_list

    def ping_pong_pinning(self, clusters: List[List[NodeInfo]], nodes_per_round: int) -> List[int]:
        """
        Generates ping-pong CPU pinning strategy.
        Processes x nodes from each cluster in round-robin fashion.
        """
        if not clusters or nodes_per_round <= 0:
            return []

        cpu_list = []

        # Determine max SMT count
        max_smt = max(len(core) for cluster in clusters
                     for node in cluster
                     for core in node.cores_list)

        # For each SMT position
        for smt_pos in range(max_smt):
            # Track current position in each cluster
            cluster_positions = [0] * len(clusters)

            # Continue until all nodes are processed
            while any(pos < len(cluster) for pos, cluster in zip(cluster_positions, clusters)):
                # Process each cluster in round-robin
                for cluster_idx, cluster in enumerate(clusters):
                    nodes_processed = 0

                    # Process up to nodes_per_round nodes from this cluster
                    while (nodes_processed < nodes_per_round and
                           cluster_positions[cluster_idx] < len(cluster)):

                        node = cluster[cluster_positions[cluster_idx]]

                        # Add CPUs for current SMT position from this node
                        for core in node.cores_list:
                            if smt_pos < len(core):
                                cpu_list.append(core[smt_pos])

                        cluster_positions[cluster_idx] += 1
                        nodes_processed += 1

        return cpu_list

    @staticmethod
    def save_pinning_to_file(filepath: str, pinning: List[int]) -> None:
        """Save CPU pinning list to a file"""
        try:
            with open(filepath, "w") as file:
                for cpu_id in pinning:
                    file.write(f"{cpu_id}\n")
        except IOError as e:
            print(f"Error writing to {filepath}: {str(e)}")
            raise


def print_cpu_info(cpu_info: List[CPUInfo]) -> None:
    """Print CPU information in a human-readable format"""
    for cpu in cpu_info:
        print(f"Logical ID: {cpu.logical_id}")
        print(f"\tCore ID:\t{cpu.core_id}")
        print(f"\tSiblings:\t{cpu.siblings}")
        print(f"\tNuma Node:\t{cpu.numa_node}")
        print(f"\tCache IDs:\t{cpu.cache_ids}")
        print(f"\tPackage ID:\t{cpu.package_id}")
        print()


def print_cluster_info(clusters: List[List[NodeInfo]], cache_level: int) -> None:
    """Print cluster information in a human-readable format"""
    print(f"Cluster Information (Cache optimization level: {cache_level}):")

    for cluster_idx, cluster in enumerate(clusters):
        print(f"\nCluster {cluster_idx} (Memory Node: {cluster[0].id}):")

        for node_idx, node in enumerate(cluster):
            print(f"  Node {node.id} ({'Memory' if node.memory else 'Compute'}):")
            print(f"    Cores: {len(node.cores_list)}")
            for core_idx, cpus in enumerate(node.cores_list):
                print(f"      Core {core_idx}: {cpus}")


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description='System Topology Information and CPU Pinning',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --cpu                          # Print CPU layout
  %(prog)s --cluster --cache 2           # Print clusters with L2 cache optimization
  %(prog)s --pin_cluster out.txt         # Generate cluster-first pinning
  %(prog)s --pin_ping_pong out.txt 2     # Generate ping-pong pinning with 2 nodes per round
        """
    )

    # Main operation modes
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--cpu', action='store_true',
                      help='Print CPU layout information')
    group.add_argument('--cluster', action='store_true',
                      help='Print cluster information')
    group.add_argument('--pin_ping_pong', nargs=2, metavar=('FILEPATH', 'X'),
                      help='Generate ping-pong pinning (FILEPATH = output file, X = nodes per round)')
    group.add_argument('--pin_cluster', nargs=1, metavar='FILEPATH',
                      help='Generate cluster-first pinning configuration')

    # Optional parameters
    parser.add_argument('--cache', type=int, default=3, metavar='LEVEL',
                      help='Cache level to optimize for (default: 3)')
    parser.add_argument('--json', action='store_true',
                      help='Output in JSON format (where applicable)')

    args = parser.parse_args()

    # Initialize system topology
    topology = SystemTopology()
    pinning_generator = CPUPinningGenerator(topology)

    try:
        if args.cpu:
            cpu_info = topology.get_cpu_info()
            if args.json:
                # Convert to JSON-serializable format
                cpu_data = [
                    {
                        'logical_id': cpu.logical_id,
                        'core_id': cpu.core_id,
                        'siblings': cpu.siblings,
                        'numa_node': cpu.numa_node,
                        'cache_ids': cpu.cache_ids,
                        'package_id': cpu.package_id
                    }
                    for cpu in cpu_info
                ]
                print(json.dumps(cpu_data, indent=2))
            else:
                print("CPU Layout Information:")
                print_cpu_info(cpu_info)

        elif args.cluster:
            clusters = topology.get_clusters(optimize_cache_level=args.cache)
            if args.json:
                # Convert to JSON-serializable format
                cluster_data = [
                    [
                        {
                            'id': node.id,
                            'memory': node.memory,
                            'cores_list': [list(core) for core in node.cores_list],
                            'distance': node.distance
                        }
                        for node in cluster
                    ]
                    for cluster in clusters
                ]
                print(json.dumps(cluster_data, indent=2))
            else:
                print_cluster_info(clusters, args.cache)

        elif args.pin_ping_pong:
            filepath = args.pin_ping_pong[0]
            try:
                x = int(args.pin_ping_pong[1])
                if x <= 0:
                    raise ValueError("X must be a positive integer")

                clusters = topology.get_clusters(optimize_cache_level=args.cache)
                if not clusters:
                    print("Error: No clusters found")
                    return 1

                cpu_list = pinning_generator.ping_pong_pinning(clusters, x)
                pinning_generator.save_pinning_to_file(filepath, cpu_list)
                print(f"Ping-pong pinning (x={x}) saved to {filepath}")

            except ValueError as e:
                print(f"Error: {str(e)}")
                return 1

        elif args.pin_cluster:
            filepath = args.pin_cluster[0]

            clusters = topology.get_clusters(optimize_cache_level=args.cache)
            if not clusters:
                print("Error: No clusters found")
                return 1

            cpu_list = pinning_generator.cluster_first_pinning(clusters)
            pinning_generator.save_pinning_to_file(filepath, cpu_list)
            print(f"Cluster-first pinning saved to {filepath}")

    except KeyboardInterrupt:
        print("\nOperation cancelled by user")
        return 1
    except Exception as e:
        print(f"Unexpected error: {str(e)}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
